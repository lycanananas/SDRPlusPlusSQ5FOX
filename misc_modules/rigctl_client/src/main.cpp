#include <utils/proto/rigctl.h>
#include <utils/net.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <recorder_interface.h>
#include <meteor_demodulator_interface.h>
#include <config.h>
#include <cstdio>
#include <chrono>
#include <cctype>
#include <cmath>
#include <map>
#include <thread>
#include <vector>
#include <radio_interface.h>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rigctl_client",
    /* Description:     */ "Client for the RigCTL protocol",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const std::vector<std::string> SUPPORTED_MODE_NAMES = {
    "LSB",
    "USB",
    "PKTLSB",
    "PKTUSB",
    "CWR",
    "CW",
    "RTTYR",
    "RTTY",
    "AM",
    "FM"
};

class RigctlClientModule : public ModuleManager::Instance {
public:
    RigctlClientModule(std::string name) {
        this->name = name;

        // Load default
        strcpy(host, "127.0.0.1");

        // Load config
        config.acquire();
        if (config.conf[name].contains("host")) {
            std::string h = config.conf[name]["host"];
            strcpy(host, h.c_str());
        }
        if (config.conf[name].contains("port")) {
            port = config.conf[name]["port"];
            port = std::clamp<int>(port, 1, 65535);
        }
        if (config.conf[name].contains("ifFreq")) {
            ifFreq = config.conf[name]["ifFreq"];
        }
        if (config.conf[name].contains("pollIntervalMs")) {
            pollIntervalMs = std::clamp<int>(config.conf[name]["pollIntervalMs"], 10, 10000);
        }
        if (config.conf[name].contains("syncRadioMode")) {
            syncRadioMode = config.conf[name]["syncRadioMode"];
        }
        loadModeOffsets();
        config.release();

        syncIfFreqInput();

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~RigctlClientModule() {
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {
        
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (running) { return; }

        sigpath::sourceManager.setPanadapterIF(ifFreq);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);

        lastRigMode = "Unknown";
        lastRigFrequency = 0.0;
        hasRigFrequency = false;
        reconnecting = true;
        nextReconnectAt = std::chrono::steady_clock::now();
        running = true;
        workerThread = std::thread(&RigctlClientModule::worker, this);
    }

    void stop() {
        std::shared_ptr<net::Socket> socket;
        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            if (!running) { return; }
            running = false;
            socket = rigSocket;
            rigSocket.reset();
            reconnecting = false;
        }

        if (socket) {
            socket->close();
        }
        if (workerThread.joinable()) {
            workerThread.join();
        }

        std::lock_guard<std::recursive_mutex> lck(mtx);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);
    }

private:
    static void menuHandler(void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool running = false;
        bool connected = false;
        bool reconnecting = false;
        double rigFrequency = 0.0;
        double effectiveIfFrequency = 0.0;
        bool hasRigFrequency = false;
        std::string rigMode;
        bool syncRadioMode = false;

        {
            std::lock_guard<std::recursive_mutex> lck(_this->mtx);
            running = _this->running;
            connected = (_this->rigSocket && _this->rigSocket->isOpen());
            reconnecting = _this->reconnecting;
            rigFrequency = _this->lastRigFrequency;
            effectiveIfFrequency = _this->lastEffectiveIfFrequency;
            hasRigFrequency = _this->hasRigFrequency;
            rigMode = _this->lastRigMode;
            syncRadioMode = _this->syncRadioMode;
        }

        if (running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigctl_cli_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigctl_cli_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (running) { style::endDisabled(); }

        ImGui::LeftLabel("Poll Interval (ms)");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_rigctl_poll_interval_", _this->name), &_this->pollIntervalMs, 0, 0)) {
            _this->pollIntervalMs = std::clamp<int>(_this->pollIntervalMs, 10, 10000);
            config.acquire();
            config.conf[_this->name]["pollIntervalMs"] = _this->pollIntervalMs;
            config.release(true);
        }

        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        ImGui::InputText(CONCAT("##_rigctl_if_freq_", _this->name), _this->ifFreqInput, sizeof(_this->ifFreqInput));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (!_this->applyIfFreqInput()) {
                _this->syncIfFreqInput();
            }
        }

        if (ImGui::Checkbox(CONCAT("Sync radio mode##_rigctl_sync_mode_", _this->name), &_this->syncRadioMode)) {
            config.acquire();
            config.conf[_this->name]["syncRadioMode"] = _this->syncRadioMode;
            config.release(true);
        }

        if (ImGui::CollapsingHeader(CONCAT("Mode Offsets (Hz)##_rigctl_mode_offsets_", _this->name))) {
            for (const auto& modeName : SUPPORTED_MODE_NAMES) {
                _this->drawModeOffsetEditor(modeName);
            }
        }

        ImGui::FillWidth();
        if (running && ImGui::Button(CONCAT("Stop##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!running && ImGui::Button(CONCAT("Start##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (connected && running) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
        }
        else if (running && reconnecting) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Reconnecting");
        }
        else if (running) {
            ImGui::TextColored(ImVec4(1.0, 0.4, 0.0, 1.0), "Disconnected");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }

        if (hasRigFrequency) {
            std::string formattedRadioFrequency = _this->formatFrequency(rigFrequency);
            ImGui::Text("Radio Frequency: %s", formattedRadioFrequency.c_str());
        }
        else {
            ImGui::TextUnformatted("Radio Frequency: N/A");
        }
        ImGui::Text("Radio Mode: %s", rigMode.c_str());
        if (hasRigFrequency) {
            std::string formattedIfFrequency = _this->formatFrequency(effectiveIfFrequency);
            ImGui::Text("Effective IF: %s", formattedIfFrequency.c_str());
        }
        else {
            ImGui::TextUnformatted("Effective IF: N/A");
        }
    }

    void worker() {
        while (true) {
            int interval = 100;
            {
                std::lock_guard<std::recursive_mutex> lck(mtx);
                if (!running) { break; }
                interval = pollIntervalMs;
            }

            ensureConnected();
            pollRigState();
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }

    void ensureConnected() {
        std::string currentHost;
        int currentPort = 0;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            if (!running) { return; }
            if (rigSocket && rigSocket->isOpen()) {
                reconnecting = false;
                return;
            }
            if (now < nextReconnectAt) {
                reconnecting = true;
                return;
            }
            currentHost = host;
            currentPort = port;
            nextReconnectAt = now + std::chrono::seconds(1);
            reconnecting = true;
        }

        try {
            auto socket = net::connect(currentHost, currentPort);
            std::lock_guard<std::recursive_mutex> lck(mtx);
            if (!running) {
                socket->close();
                return;
            }
            rigSocket = socket;
            reconnecting = false;
        }
        catch (const std::exception&) {
        }
    }

    void pollRigState() {
        std::shared_ptr<net::Socket> socket;
        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            socket = rigSocket;
        }

        if (!socket || !socket->isOpen()) { return; }

        double frequency = 0.0;
        std::string mode;
        if (!queryFrequency(socket, frequency) || !queryMode(socket, mode)) {
            socket->close();
            std::lock_guard<std::recursive_mutex> lck(mtx);
            reconnecting = true;
            return;
        }

        applyDisplayState(frequency, mode);
    }

    bool queryFrequency(const std::shared_ptr<net::Socket>& socket, double& frequency) {
        socket->sendstr("f\n");

        std::string line;
        if (socket->recvline(line, 0, 1000) <= 0) { return false; }

        try {
            frequency = std::stod(line);
        }
        catch (...) {
            return false;
        }

        return true;
    }

    bool queryMode(const std::shared_ptr<net::Socket>& socket, std::string& mode) {
        socket->sendstr("m\n");

        std::string line;
        if (socket->recvline(line, 0, 1000) <= 0) { return false; }

        mode = extractModeToken(line);

        std::string extraLine;
        int extraLineLen = socket->recvline(extraLine, 0, 10);
        if (mode.empty() && extraLineLen > 0) {
            mode = extractModeToken(extraLine);
        }

        return !mode.empty();
    }

    std::string extractModeToken(const std::string& line) {
        std::string token;

        for (char ch : line) {
            unsigned char uch = static_cast<unsigned char>(ch);
            if (std::isalpha(uch)) {
                token += std::toupper(uch);
                continue;
            }
            if (!token.empty()) {
                break;
            }
            if (!std::isspace(uch)) {
                return "";
            }
        }

        if (token == "RPRT") {
            return "";
        }
        return token;
    }

    std::string formatFrequency(double frequency) const {
        double absFrequency = std::abs(frequency);
        if (absFrequency >= 1000000.0) {
            return formatScaledFrequency(frequency / 1000000.0, "MHz", 6);
        }
        if (absFrequency >= 1000.0) {
            return formatScaledFrequency(frequency / 1000.0, "kHz", 3);
        }
        return formatScaledFrequency(frequency, "Hz", 0);
    }

    std::string formatScaledFrequency(double value, const char* unit, int precision) const {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);

        std::string text = buffer;
        if (precision > 0) {
            while (!text.empty() && text.back() == '0') {
                text.pop_back();
            }
            if (!text.empty() && text.back() == '.') {
                text.pop_back();
            }
        }

        return text + " " + unit;
    }

    bool applyIfFreqInput() {
        double parsedFrequency = 0.0;
        if (!parseFrequencyInput(ifFreqInput, parsedFrequency)) {
            return false;
        }

        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            ifFreq = parsedFrequency;
            if (running) {
                sigpath::sourceManager.setPanadapterIF(ifFreq);
            }
        }

        config.acquire();
        config.conf[name]["ifFreq"] = parsedFrequency;
        config.release(true);
        syncIfFreqInput();
        return true;
    }

    bool parseFrequencyInput(const char* input, double& parsedFrequency) const {
        std::string text = input;
        trim(text);
        if (text.empty()) { return false; }

        size_t numericLength = 0;
        try {
            double value = std::stod(text, &numericLength);
            std::string suffix = text.substr(numericLength);
            trim(suffix);
            for (char& ch : suffix) {
                ch = std::tolower(static_cast<unsigned char>(ch));
            }

            double multiplier = 1.0;
            if (suffix.empty() || suffix == "hz") {
                multiplier = 1.0;
            }
            else if (suffix == "k" || suffix == "khz") {
                multiplier = 1000.0;
            }
            else if (suffix == "m" || suffix == "mhz") {
                multiplier = 1000000.0;
            }
            else {
                return false;
            }

            parsedFrequency = value * multiplier;
            return std::isfinite(parsedFrequency) && parsedFrequency >= 0.0;
        }
        catch (...) {
            return false;
        }
    }

    void syncIfFreqInput() {
        std::snprintf(ifFreqInput, sizeof(ifFreqInput), "%.6fM", ifFreq / 1000000.0);
        trimTrailingZeroes(ifFreqInput);
    }

    void trim(std::string& text) const {
        size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
            start++;
        }

        size_t end = text.size();
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            end--;
        }

        text = text.substr(start, end - start);
    }

    void trimTrailingZeroes(char* text) {
        std::string value = text;
        size_t suffixPos = value.find('M');
        if (suffixPos == std::string::npos) { return; }

        while (suffixPos > 0 && value[suffixPos - 1] == '0') {
            value.erase(suffixPos - 1, 1);
            suffixPos--;
        }
        if (suffixPos > 0 && value[suffixPos - 1] == '.') {
            value.erase(suffixPos - 1, 1);
        }

        std::snprintf(text, sizeof(ifFreqInput), "%s", value.c_str());
    }

    void applyDisplayState(double frequency, const std::string& mode) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        double effectiveIfFrequency = ifFreq + getModeOffset(mode);

        lastRigFrequency = frequency;
        lastRigMode = mode;
        lastEffectiveIfFrequency = effectiveIfFrequency;
        hasRigFrequency = true;

        sigpath::sourceManager.setPanadapterIF(effectiveIfFrequency);

        gui::waterfall.setCenterFrequency(frequency);
        gui::freqSelect.setFrequency(std::llround(frequency));
        gui::freqSelect.frequencyChanged = false;
        gui::waterfall.centerFreqMoved = false;

        if (syncRadioMode) {
            syncModeToRadioModule(mode);
        }
    }

    void drawModeOffsetEditor(const std::string& modeName) {
        ImGui::LeftLabel(modeName.c_str());
        ImGui::FillWidth();

        double offset = 0.0;
        {
            std::lock_guard<std::recursive_mutex> lck(mtx);
            offset = modeOffsetsHz[modeName];
        }

        if (ImGui::InputDouble(CONCAT("##_rigctl_mode_offset_", modeName.c_str()), &offset, 1.0, 100.0, "%.0f")) {
            {
                std::lock_guard<std::recursive_mutex> lck(mtx);
                modeOffsetsHz[modeName] = offset;
                if (running && lastRigMode == modeName) {
                    lastEffectiveIfFrequency = ifFreq + offset;
                    sigpath::sourceManager.setPanadapterIF(lastEffectiveIfFrequency);
                }
            }

            config.acquire();
            config.conf[name]["modeOffsetsHz"][modeName] = offset;
            config.release(true);
        }
    }

    void loadModeOffsets() {
        for (const auto& modeName : SUPPORTED_MODE_NAMES) {
            modeOffsetsHz[modeName] = 0.0;
        }

        if (!config.conf[name].contains("modeOffsetsHz")) {
            return;
        }

        for (const auto& modeName : SUPPORTED_MODE_NAMES) {
            if (config.conf[name]["modeOffsetsHz"].contains(modeName)) {
                modeOffsetsHz[modeName] = config.conf[name]["modeOffsetsHz"][modeName];
            }
        }
    }

    double getModeOffset(const std::string& modeName) const {
        auto it = modeOffsetsHz.find(modeName);
        if (it == modeOffsetsHz.end()) {
            return 0.0;
        }
        return it->second;
    }

    void syncModeToRadioModule(const std::string& modeName) {
        if (gui::waterfall.selectedVFO.empty()) { return; }
        if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) != "radio") { return; }

        int radioMode = mapRigctlModeToRadioMode(modeName);
        if (radioMode < 0) { return; }

        int currentRadioMode = -1;
        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &currentRadioMode);
        if (currentRadioMode == radioMode) { return; }

        core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &radioMode, NULL);
    }

    int mapRigctlModeToRadioMode(const std::string& modeName) const {
        if (modeName == "LSB" || modeName == "PKTLSB" || modeName == "RTTY") {
            return RADIO_IFACE_MODE_LSB;
        }
        if (modeName == "USB" || modeName == "PKTUSB" || modeName == "RTTYR") {
            return RADIO_IFACE_MODE_USB;
        }
        if (modeName == "CWR" || modeName == "CW") {
            return RADIO_IFACE_MODE_CW;
        }
        if (modeName == "AM") {
            return RADIO_IFACE_MODE_AM;
        }
        if (modeName == "FM") {
            return RADIO_IFACE_MODE_NFM;
        }
        return -1;
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    int pollIntervalMs = 100;
    std::shared_ptr<net::Socket> rigSocket;
    std::thread workerThread;
    std::chrono::steady_clock::time_point nextReconnectAt = std::chrono::steady_clock::time_point::min();

    double ifFreq = 8830000.0;
    char ifFreqInput[64] = "";
    double lastRigFrequency = 0.0;
    double lastEffectiveIfFrequency = 0.0;
    bool hasRigFrequency = false;
    bool reconnecting = false;
    bool syncRadioMode = false;
    std::string lastRigMode = "Unknown";
    std::map<std::string, double> modeOffsetsHz;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rigctl_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RigctlClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RigctlClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
