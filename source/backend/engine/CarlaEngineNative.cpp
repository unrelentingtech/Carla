/*
 * Carla Plugin Engine (Native)
 * Copyright (C) 2013 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#ifdef BUILD_BRIDGE
# error This file should not be compiled if building bridge
#endif

#include "CarlaEngineInternal.hpp"
#include "CarlaPipeUtils.hpp"
#include "CarlaStateUtils.hpp"

#include "CarlaNative.hpp"

#include <QtCore/QTextStream>

CARLA_BACKEND_START_NAMESPACE

// -----------------------------------------------------------------------

class CarlaEngineNativeUI : public CarlaPipeServer
{
public:
    enum UiState {
        UiNone = 0,
        UiHide,
        UiShow,
        UiCrashed
    };

    CarlaEngineNativeUI(CarlaEngine* const engine)
        : fEngine(engine),
          fUiState(UiNone)
    {
        carla_debug("CarlaEngineNativeUI::CarlaEngineNativeUI(%p)", engine);
    }

    ~CarlaEngineNativeUI() override
    {
        CARLA_ASSERT_INT(fUiState == UiNone, fUiState);
        carla_debug("CarlaEngineNativeUI::~CarlaEngineNativeUI()");
    }

    void setData(const char* const filename, const double sampleRate, const char* const uiTitle)
    {
        fFilename   = filename;
        fSampleRate = CarlaString(sampleRate);
        fUiTitle    = uiTitle;
    }

    UiState getAndResetUiState() noexcept
    {
        const UiState uiState(fUiState);
        fUiState = UiNone;
        return uiState;
    }

    void start()
    {
        CarlaPipeServer::start(fFilename, fSampleRate, fUiTitle);
        writeMsg("show\n", 5);
    }

protected:
    void msgReceived(const char* const msg) override
    {
        /*
         * TODO:
         * load_file load_project save_project patchbay_connect patchbay_disconnect patchbay_refresh
         * transport_play transport_pause transport_relocate
         * *add_plugin* remove_plugin remove_all_plugins rename_plugin clone_plugin replace_plugin switch_plugins
         * load_plugin_state save_plugin_state
         * set_option *set_active* set_drywet set_volume set_balance_left set_balance_right set_panning set_ctrl_channel
         * set_parameter_value set_parameter_midi_channel set_parameter_midi_cc set_program set_midi_program set_custom_data set_chunk_data
         * prepare_for_save send_midi_note show_custom_ui
         */

        if (std::strcmp(msg, "exiting") == 0)
        {
            waitChildClose();
            fUiState = UiHide;
        }
        else if (std::strcmp(msg, "set_engine_option") == 0)
        {
            int option, value;
            const char* valueStr;

            CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(option),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(value),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(valueStr),);

            fEngine->setOption((EngineOption)option, value, valueStr);
        }
        else if (std::strcmp(msg, "add_plugin") == 0)
        {
            int btype, ptype;
            const char* filename;
            const char* name;
            const char* label;

            CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(btype),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(ptype),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(filename),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(name),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsString(label),);

            fEngine->addPlugin((BinaryType)btype, (PluginType)ptype, filename, name, label);
        }
        else if (std::strcmp(msg, "set_active") == 0)
        {
            int pluginId;
            bool onOff;

            CARLA_SAFE_ASSERT_RETURN(readNextLineAsInt(pluginId),);
            CARLA_SAFE_ASSERT_RETURN(readNextLineAsBool(onOff),);

            if (CarlaPlugin* const plugin = fEngine->getPlugin(pluginId))
                plugin->setActive(onOff, true, false);
        }
        else
        {
            carla_stderr("msgReceived : %s", msg);
        }
    }

private:
    CarlaEngine* const fEngine;

    CarlaString fFilename;
    CarlaString fSampleRate;
    CarlaString fUiTitle;
    UiState     fUiState;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineNativeUI)
};

// -----------------------------------------------------------------------

class CarlaEngineNative : public CarlaEngine
{
public:
    CarlaEngineNative(const NativeHostDescriptor* const host, const bool isPatchbay)
        : CarlaEngine(),
          pHost(host),
          fIsPatchbay(isPatchbay),
          fIsActive(false),
          fIsRunning(false),
          fUiServer(this)
    {
        carla_debug("CarlaEngineNative::CarlaEngineNative()");

        // set-up engine
        if (fIsPatchbay)
        {
            pData->options.processMode         = ENGINE_PROCESS_MODE_PATCHBAY;
            pData->options.transportMode       = ENGINE_TRANSPORT_MODE_PLUGIN;
            pData->options.forceStereo         = false;
            pData->options.preferPluginBridges = false;
            pData->options.preferUiBridges     = false;
            init("Carla-Patchbay");
        }
        else
        {
            pData->options.processMode         = ENGINE_PROCESS_MODE_CONTINUOUS_RACK;
            pData->options.transportMode       = ENGINE_TRANSPORT_MODE_PLUGIN;
            pData->options.forceStereo         = true;
            pData->options.preferPluginBridges = false;
            pData->options.preferUiBridges     = false;
            init("Carla-Rack");
        }

        setCallback(_ui_server_callback, this);
    }

    ~CarlaEngineNative() override
    {
        CARLA_ASSERT(! fIsActive);
        carla_debug("CarlaEngineNative::~CarlaEngineNative()");

        pData->aboutToClose = true;
        fIsRunning = false;

        removeAllPlugins();
        runPendingRtEvents();
        close();
    }

protected:
    // -------------------------------------
    // CarlaEngine virtual calls

    bool init(const char* const clientName) override
    {
        carla_debug("CarlaEngineNative::init(\"%s\")", clientName);

        pData->bufferSize = pHost->get_buffer_size(pHost->handle);
        pData->sampleRate = pHost->get_sample_rate(pHost->handle);

        fIsRunning = true;
        CarlaEngine::init(clientName);
        return true;
    }

    bool isRunning() const noexcept override
    {
        return fIsRunning;
    }

    bool isOffline() const noexcept override
    {
        return pHost->is_offline(pHost->handle);
    }

    EngineType getType() const noexcept override
    {
        return kEngineTypePlugin;
    }

    const char* getCurrentDriverName() const noexcept override
    {
        return "Plugin";
    }

    // -------------------------------------------------------------------

    void bufferSizeChanged(const uint32_t newBufferSize)
    {
        pData->bufferSize = newBufferSize;
        CarlaEngine::bufferSizeChanged(newBufferSize);
    }

    void sampleRateChanged(const double newSampleRate)
    {
        pData->sampleRate = newSampleRate;
        CarlaEngine::sampleRateChanged(newSampleRate);
    }

    // -------------------------------------------------------------------

    void uiServerCallback(const EngineCallbackOpcode action, const uint pluginId, const int value1, const int value2, const float value3, const char* const valueStr)
    {
        if (! fIsRunning)
            return;

        char strBuf[STR_MAX+1];
        std::sprintf(strBuf, "ENGINE_CALLBACK_%i\n", int(action));
        fUiServer.writeMsg(strBuf);

        std::sprintf(strBuf, "%u\n", pluginId);
        fUiServer.writeMsg(strBuf);

        std::sprintf(strBuf, "%i\n", value1);
        fUiServer.writeMsg(strBuf);

        std::sprintf(strBuf, "%i\n", value2);
        fUiServer.writeMsg(strBuf);

        std::sprintf(strBuf, "%f\n", value3);
        fUiServer.writeMsg(strBuf);

        fUiServer.writeAndFixMsg(valueStr);
    }

    // -------------------------------------------------------------------
    // Plugin parameter calls

    uint32_t getParameterCount() const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
            return plugin->getParameterCount();

        return 0;
    }

    const NativeParameter* getParameterInfo(const uint32_t index) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
            {
                static NativeParameter param;
                static char strBufName[STR_MAX+1];
                static char strBufUnit[STR_MAX+1];

                const ParameterData& paramData(plugin->getParameterData(index));
                const ParameterRanges& paramRanges(plugin->getParameterRanges(index));

                plugin->getParameterName(index, strBufName);
                plugin->getParameterUnit(index, strBufUnit);

                unsigned int hints = 0x0;

                if (paramData.hints & PARAMETER_IS_BOOLEAN)
                    hints |= ::PARAMETER_IS_BOOLEAN;
                if (paramData.hints & PARAMETER_IS_INTEGER)
                    hints |= ::PARAMETER_IS_INTEGER;
                if (paramData.hints & PARAMETER_IS_LOGARITHMIC)
                    hints |= ::PARAMETER_IS_LOGARITHMIC;
                if (paramData.hints & PARAMETER_IS_AUTOMABLE)
                    hints |= ::PARAMETER_IS_AUTOMABLE;
                if (paramData.hints & PARAMETER_USES_SAMPLERATE)
                    hints |= ::PARAMETER_USES_SAMPLE_RATE;
                if (paramData.hints & PARAMETER_USES_SCALEPOINTS)
                    hints |= ::PARAMETER_USES_SCALEPOINTS;
                if (paramData.hints & PARAMETER_USES_CUSTOM_TEXT)
                    hints |= ::PARAMETER_USES_CUSTOM_TEXT;

                if (paramData.type == PARAMETER_INPUT || paramData.type == PARAMETER_OUTPUT)
                {
                    if (paramData.hints & PARAMETER_IS_ENABLED)
                        hints |= ::PARAMETER_IS_ENABLED;
                    if (paramData.type == PARAMETER_OUTPUT)
                        hints |= ::PARAMETER_IS_OUTPUT;
                }

                param.hints = static_cast<NativeParameterHints>(hints);
                param.name  = strBufName;
                param.unit  = strBufUnit;
                param.ranges.def = paramRanges.def;
                param.ranges.min = paramRanges.min;
                param.ranges.max = paramRanges.max;
                param.ranges.step = paramRanges.step;
                param.ranges.stepSmall = paramRanges.stepSmall;
                param.ranges.stepLarge = paramRanges.stepLarge;
                param.scalePointCount = 0; // TODO
                param.scalePoints = nullptr;

                return &param;
            }
        }

        return nullptr;
    }

    float getParameterValue(const uint32_t index) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
                return plugin->getParameterValue(index);
        }

        return 0.0f;
    }

    const char* getParameterText(const uint32_t index, const float value) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
            {
                static char strBuf[STR_MAX+1];
                carla_zeroChar(strBuf, STR_MAX+1);

                plugin->getParameterText(index, value, strBuf);

                return strBuf;
            }
        }

        return nullptr;
    }

    // -------------------------------------------------------------------
    // Plugin midi-program calls

    uint32_t getMidiProgramCount() const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
           return plugin->getMidiProgramCount();

        return 0;
    }

    const NativeMidiProgram* getMidiProgramInfo(const uint32_t index) const
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getMidiProgramCount())
            {
                static NativeMidiProgram midiProg;

                {
                    const MidiProgramData& midiProgData(plugin->getMidiProgramData(index));

                    midiProg.bank    = midiProgData.bank;
                    midiProg.program = midiProgData.program;
                    midiProg.name    = midiProgData.name;
                }

                return &midiProg;
            }
        }

        return nullptr;
    }

    // -------------------------------------------------------------------
    // Plugin state calls

    void setParameterValue(const uint32_t index, const float value)
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
        {
            if (index < plugin->getParameterCount())
                plugin->setParameterValue(index, value, false, false, false);
        }
    }

    void setMidiProgram(const uint8_t, const uint32_t bank, const uint32_t program)
    {
        if (CarlaPlugin* const plugin = _getFirstPlugin())
            plugin->setMidiProgramById(bank, program, false, false, false);
    }

    // -------------------------------------------------------------------
    // Plugin process calls

    void activate()
    {
#if 0
        for (uint32_t i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin == nullptr || ! plugin->isEnabled())
                continue;

            plugin->setActive(true, true, false);
        }
#endif
        fIsActive = true;
    }

    void deactivate()
    {
        fIsActive = false;
#if 0
        for (uint32_t i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin == nullptr || ! plugin->isEnabled())
                continue;

            plugin->setActive(false, true, false);
        }
#endif

        // just in case
        runPendingRtEvents();
    }

    void process(float** const inBuffer, float** const outBuffer, const uint32_t frames, const NativeMidiEvent* const midiEvents, const uint32_t midiEventCount)
    {
        if (pData->curPluginCount == 0 && ! fIsPatchbay)
        {
            FLOAT_COPY(outBuffer[0], inBuffer[0], frames);
            FLOAT_COPY(outBuffer[1], inBuffer[1], frames);

            return runPendingRtEvents();;
        }

        // ---------------------------------------------------------------
        // Time Info

        const NativeTimeInfo* const timeInfo(pHost->get_time_info(pHost->handle));

        pData->timeInfo.playing = timeInfo->playing;
        pData->timeInfo.frame   = timeInfo->frame;
        pData->timeInfo.usecs   = timeInfo->usecs;
        pData->timeInfo.valid   = 0x0;

        if (timeInfo->bbt.valid)
        {
            pData->timeInfo.valid |= EngineTimeInfo::kValidBBT;

            pData->timeInfo.bbt.bar = timeInfo->bbt.bar;
            pData->timeInfo.bbt.beat = timeInfo->bbt.beat;
            pData->timeInfo.bbt.tick = timeInfo->bbt.tick;
            pData->timeInfo.bbt.barStartTick = timeInfo->bbt.barStartTick;

            pData->timeInfo.bbt.beatsPerBar = timeInfo->bbt.beatsPerBar;
            pData->timeInfo.bbt.beatType = timeInfo->bbt.beatType;

            pData->timeInfo.bbt.ticksPerBeat = timeInfo->bbt.ticksPerBeat;
            pData->timeInfo.bbt.beatsPerMinute = timeInfo->bbt.beatsPerMinute;
        }

        // ---------------------------------------------------------------
        // initialize events

        carla_zeroStruct<EngineEvent>(pData->bufEvents.in,  kEngineMaxInternalEventCount);
        carla_zeroStruct<EngineEvent>(pData->bufEvents.out, kEngineMaxInternalEventCount);

        // ---------------------------------------------------------------
        // events input (before processing)

        {
            uint32_t engineEventIndex = 0;

            for (uint32_t i=0; i < midiEventCount && engineEventIndex < kEngineMaxInternalEventCount; ++i)
            {
                const NativeMidiEvent& midiEvent(midiEvents[i]);
                EngineEvent&           engineEvent(pData->bufEvents.in[engineEventIndex++]);

                engineEvent.time = midiEvent.time;
                engineEvent.fillFromMidiData(midiEvent.size, midiEvent.data);

                if (engineEventIndex >= kEngineMaxInternalEventCount)
                    break;
            }
        }

        if (fIsPatchbay)
        {
            // -----------------------------------------------------------
            // create audio buffers

            //float*   inBuf[2]    = { inBuffer[0], inBuffer[1] };
            //float*   outBuf[2]   = { outBuffer[0], outBuffer[1] };
            //uint32_t bufCount[2] = { 2, 2 };

            // -----------------------------------------------------------
            // process

            //pData->processPatchbay(inBuf, outBuf, bufCount, frames, isOffline());
        }
        else
        {
            // -----------------------------------------------------------
            // create audio buffers

            float* inBuf[2]  = { inBuffer[0], inBuffer[1] };
            float* outBuf[2] = { outBuffer[0], outBuffer[1] };

            // -----------------------------------------------------------
            // process

            pData->processRack(inBuf, outBuf, frames, isOffline());
        }

        // ---------------------------------------------------------------
        // events output (after processing)

        carla_zeroStruct<EngineEvent>(pData->bufEvents.in, kEngineMaxInternalEventCount);

        {
            NativeMidiEvent midiEvent;

            for (uint32_t i=0; i < kEngineMaxInternalEventCount; ++i)
            {
                const EngineEvent& engineEvent(pData->bufEvents.out[i]);

                if (engineEvent.type == kEngineEventTypeNull)
                    break;

                midiEvent.time = engineEvent.time;

                if (engineEvent.type == CarlaBackend::kEngineEventTypeControl)
                {
                    midiEvent.port = 0;
                    engineEvent.ctrl.dumpToMidiData(engineEvent.channel, midiEvent.size, midiEvent.data);
                }
                else if (engineEvent.type == kEngineEventTypeMidi)
                {
                    if (engineEvent.midi.size > 4 || engineEvent.midi.dataExt != nullptr)
                        continue;

                    midiEvent.port = engineEvent.midi.port;
                    midiEvent.size = engineEvent.midi.size;

                    midiEvent.data[0] = static_cast<uint8_t>(engineEvent.midi.data[0] + engineEvent.channel);

                    for (uint8_t j=1; j < midiEvent.size; ++j)
                        midiEvent.data[j] = engineEvent.midi.data[j];
                }
                else
                {
                    carla_stderr("Unknown event type...");
                    continue;
                }

                pHost->write_midi_event(pHost->handle, &midiEvent);
            }
        }

        runPendingRtEvents();
    }

    // -------------------------------------------------------------------
    // Plugin UI calls

    void uiShow(const bool show)
    {
        if (show)
        {
            fUiServer.setData("/home/falktx/FOSS/GIT-mine/Carla/source/carla-plugin", pData->sampleRate, pHost->uiName);
            fUiServer.start();
        }
        else
        {
            fUiServer.stop();
        }
    }

    void uiIdle()
    {
        CarlaEngine::idle();
        fUiServer.idle();

        switch (fUiServer.getAndResetUiState())
        {
        case CarlaEngineNativeUI::UiNone:
        case CarlaEngineNativeUI::UiShow:
            break;
        case CarlaEngineNativeUI::UiCrashed:
            pHost->dispatcher(pHost->handle, HOST_OPCODE_UI_UNAVAILABLE, 0, 0, nullptr, 0.0f);
            break;
        case CarlaEngineNativeUI::UiHide:
            pHost->ui_closed(pHost->handle);
            break;
        }
    }

    // -------------------------------------------------------------------
    // Plugin state calls

    char* getState() const
    {
        QString string;
        QTextStream out(&string);
        out << "<?xml version='1.0' encoding='UTF-8'?>\n";
        out << "<!DOCTYPE CARLA-PROJECT>\n";
        out << "<CARLA-PROJECT VERSION='2.0'>\n";

        bool firstPlugin = true;
        char strBuf[STR_MAX+1];

        for (unsigned int i=0; i < pData->curPluginCount; ++i)
        {
            CarlaPlugin* const plugin(pData->plugins[i].plugin);

            if (plugin != nullptr && plugin->isEnabled())
            {
                if (! firstPlugin)
                    out << "\n";

                strBuf[0] = '\0';

                plugin->getRealName(strBuf);

                if (strBuf[0] != '\0')
                    out << QString(" <!-- %1 -->\n").arg(xmlSafeString(strBuf, true));

                QString content;
                fillXmlStringFromSaveState(content, plugin->getSaveState());

                out << " <Plugin>\n";
                out << content;
                out << " </Plugin>\n";

                firstPlugin = false;
            }
        }

        out << "</CARLA-PROJECT>\n";

        return strdup(string.toUtf8().constData());
    }

    void setState(const char* const data)
    {
        QDomDocument xml;
        xml.setContent(QString(data));

        QDomNode xmlNode(xml.documentElement());

        if (xmlNode.toElement().tagName().compare("carla-project", Qt::CaseInsensitive) != 0)
        {
            carla_stderr2("Not a valid Carla project");
            return;
        }

        bool pluginsAdded = false;

        for (QDomNode node = xmlNode.firstChild(); ! node.isNull(); node = node.nextSibling())
        {
            if (node.toElement().tagName().compare("plugin", Qt::CaseInsensitive) == 0)
            {
                SaveState saveState;
                fillSaveStateFromXmlNode(saveState, node);

                CARLA_SAFE_ASSERT_CONTINUE(saveState.type != nullptr)

                const void* extraStuff = nullptr;

                // check if using GIG, SF2 or SFZ 16outs
                static const char kUse16OutsSuffix[] = " (16 outs)";

                if (CarlaString(saveState.label).endsWith(kUse16OutsSuffix))
                {
                    if (std::strcmp(saveState.type, "GIG") == 0 || std::strcmp(saveState.type, "SF2") == 0)
                        extraStuff = "true";
                }

                // TODO - proper find&load plugins

                if (addPlugin(getPluginTypeFromString(saveState.type), saveState.binary, saveState.name, saveState.label, extraStuff))
                {
                    if (CarlaPlugin* const plugin = getPlugin(pData->curPluginCount-1))
                        plugin->loadSaveState(saveState);
                }

                pluginsAdded = true;
            }
        }

        if (pluginsAdded)
            pHost->dispatcher(pHost->handle, HOST_OPCODE_RELOAD_ALL, 0, 0, nullptr, 0.0f);
    }

    // -------------------------------------------------------------------

public:
    #define handlePtr ((CarlaEngineNative*)handle)

    static NativePluginHandle _instantiateRack(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, false);
    }

#ifdef HAVE_JUCE
    static NativePluginHandle _instantiatePatchbay(const NativeHostDescriptor* host)
    {
        return new CarlaEngineNative(host, true);
    }
#endif

    static void _cleanup(NativePluginHandle handle)
    {
        delete handlePtr;
    }

    static uint32_t _get_parameter_count(NativePluginHandle handle)
    {
        return handlePtr->getParameterCount();
    }

    static const NativeParameter* _get_parameter_info(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getParameterInfo(index);
    }

    static float _get_parameter_value(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getParameterValue(index);
    }

    static const char* _get_parameter_text(NativePluginHandle handle, uint32_t index, float value)
    {
        return handlePtr->getParameterText(index, value);
    }

    static uint32_t _get_midi_program_count(NativePluginHandle handle)
    {
        return handlePtr->getMidiProgramCount();
    }

    static const NativeMidiProgram* _get_midi_program_info(NativePluginHandle handle, uint32_t index)
    {
        return handlePtr->getMidiProgramInfo(index);
    }

    static void _set_parameter_value(NativePluginHandle handle, uint32_t index, float value)
    {
        handlePtr->setParameterValue(index, value);
    }

    static void _set_midi_program(NativePluginHandle handle, uint8_t channel, uint32_t bank, uint32_t program)
    {
        handlePtr->setMidiProgram(channel, bank, program);
    }

    static void _ui_show(NativePluginHandle handle, bool show)
    {
        handlePtr->uiShow(show);
    }

    static void _ui_idle(NativePluginHandle handle)
    {
        handlePtr->uiIdle();
    }

    static void _activate(NativePluginHandle handle)
    {
        handlePtr->activate();
    }

    static void _deactivate(NativePluginHandle handle)
    {
        handlePtr->deactivate();
    }

    static void _process(NativePluginHandle handle, float** inBuffer, float** outBuffer, const uint32_t frames, const NativeMidiEvent* midiEvents, uint32_t midiEventCount)
    {
        handlePtr->process(inBuffer, outBuffer, frames, midiEvents, midiEventCount);
    }

    static char* _get_state(NativePluginHandle handle)
    {
        return handlePtr->getState();
    }

    static void _set_state(NativePluginHandle handle, const char* data)
    {
        handlePtr->setState(data);
    }

    static intptr_t _dispatcher(NativePluginHandle handle, NativePluginDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt)
    {
        switch(opcode)
        {
        case PLUGIN_OPCODE_NULL:
            return 0;
        case PLUGIN_OPCODE_BUFFER_SIZE_CHANGED:
            CARLA_SAFE_ASSERT_RETURN(value > 0, 0);
            handlePtr->bufferSizeChanged(static_cast<uint32_t>(value));
            return 0;
        case PLUGIN_OPCODE_SAMPLE_RATE_CHANGED:
            handlePtr->sampleRateChanged(static_cast<double>(opt));
            return 0;
        case PLUGIN_OPCODE_OFFLINE_CHANGED:
            handlePtr->offlineModeChanged(value != 0);
            return 0;
        case PLUGIN_OPCODE_UI_NAME_CHANGED:
            //handlePtr->uiNameChanged(static_cast<const char*>(ptr));
            return 0;
        }

        return 0;

        // unused
        (void)index;
        (void)ptr;
    }

    // -------------------------------------------------------------------

    static void _ui_server_callback(void* handle, EngineCallbackOpcode action, uint pluginId, int value1, int value2, float value3, const char* valueStr)
    {
        handlePtr->uiServerCallback(action, pluginId, value1, value2, value3, valueStr);
    }

    // -------------------------------------------------------------------

    #undef handlePtr

private:
    const NativeHostDescriptor* const pHost;

    const bool fIsPatchbay; // rack if false
    bool fIsActive, fIsRunning;
    CarlaEngineNativeUI fUiServer;

    CarlaPlugin* _getFirstPlugin() const noexcept
    {
        if (pData->curPluginCount == 0 || pData->plugins == nullptr)
            return nullptr;

        CarlaPlugin* const plugin(pData->plugins[0].plugin);

        if (plugin == nullptr || ! plugin->isEnabled())
            return nullptr;

        return pData->plugins[0].plugin;
    }

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineNative)
};

// -----------------------------------------------------------------------

static const NativePluginDescriptor carlaRackDesc = {
    /* category  */ ::PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(::PLUGIN_IS_SYNTH|::PLUGIN_HAS_UI|::PLUGIN_NEEDS_FIXED_BUFFERS|::PLUGIN_NEEDS_SINGLE_THREAD|::PLUGIN_USES_STATE|::PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(::PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ 0,
    /* paramOuts */ 0,
    /* name      */ "Carla-Rack",
    /* label     */ "carla-rack",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiateRack,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_parameter_text,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher
};

#ifdef HAVE_JUCE
static const NativePluginDescriptor carlaPatchbayDesc = {
    /* category  */ ::PLUGIN_CATEGORY_OTHER,
    /* hints     */ static_cast<NativePluginHints>(::PLUGIN_IS_SYNTH|::PLUGIN_HAS_UI|::PLUGIN_NEEDS_FIXED_BUFFERS|::PLUGIN_NEEDS_SINGLE_THREAD|::PLUGIN_USES_STATE|::PLUGIN_USES_TIME),
    /* supports  */ static_cast<NativePluginSupports>(::PLUGIN_SUPPORTS_EVERYTHING),
    /* audioIns  */ 2,
    /* audioOuts */ 2,
    /* midiIns   */ 1,
    /* midiOuts  */ 1,
    /* paramIns  */ 0,
    /* paramOuts */ 0,
    /* name      */ "Carla-Patchbay",
    /* label     */ "carla-patchbay",
    /* maker     */ "falkTX",
    /* copyright */ "GNU GPL v2+",
    CarlaEngineNative::_instantiatePatchbay,
    CarlaEngineNative::_cleanup,
    CarlaEngineNative::_get_parameter_count,
    CarlaEngineNative::_get_parameter_info,
    CarlaEngineNative::_get_parameter_value,
    CarlaEngineNative::_get_parameter_text,
    CarlaEngineNative::_get_midi_program_count,
    CarlaEngineNative::_get_midi_program_info,
    CarlaEngineNative::_set_parameter_value,
    CarlaEngineNative::_set_midi_program,
    /* _set_custom_data        */ nullptr,
    CarlaEngineNative::_ui_show,
    CarlaEngineNative::_ui_idle,
    /* _ui_set_parameter_value */ nullptr,
    /* _ui_set_midi_program    */ nullptr,
    /* _ui_set_custom_data     */ nullptr,
    CarlaEngineNative::_activate,
    CarlaEngineNative::_deactivate,
    CarlaEngineNative::_process,
    CarlaEngineNative::_get_state,
    CarlaEngineNative::_set_state,
    CarlaEngineNative::_dispatcher
};
#endif

// -----------------------------------------------------------------------

CARLA_BACKEND_END_NAMESPACE

CARLA_EXPORT
void carla_register_native_plugin_carla()
{
    CARLA_BACKEND_USE_NAMESPACE
    carla_register_native_plugin(&carlaRackDesc);
#ifdef HAVE_JUCE
    carla_register_native_plugin(&carlaPatchbayDesc);
#endif
}

// -----------------------------------------------------------------------
// Extra stuff for linking purposes

#ifdef CARLA_PLUGIN_EXPORT

CARLA_BACKEND_START_NAMESPACE

CarlaEngine* CarlaEngine::newJack() { return nullptr; }

CarlaEngine*       CarlaEngine::newRtAudio(const AudioApi) { return nullptr; }
unsigned int       CarlaEngine::getRtAudioApiCount() { return 0; }
const char*        CarlaEngine::getRtAudioApiName(const unsigned int) { return nullptr; }
const char* const* CarlaEngine::getRtAudioApiDeviceNames(const unsigned int) { return nullptr; }
const EngineDriverDeviceInfo* CarlaEngine::getRtAudioDeviceInfo(const unsigned int, const char* const) { return nullptr; }

# ifdef HAVE_JUCE
CarlaEngine*       CarlaEngine::newJuce(const AudioApi) { return nullptr; }
unsigned int       CarlaEngine::getJuceApiCount() { return 0; }
const char*        CarlaEngine::getJuceApiName(const unsigned int) { return nullptr; }
const char* const* CarlaEngine::getJuceApiDeviceNames(const unsigned int) { return nullptr; }
const EngineDriverDeviceInfo* CarlaEngine::getJuceDeviceInfo(const unsigned int, const char* const) { return nullptr; }
# endif

CARLA_BACKEND_END_NAMESPACE

#endif

// -----------------------------------------------------------------------
