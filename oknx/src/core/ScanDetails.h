/**
 * @file        ScanDetails.h
 * @brief       Read the identity of found devices over several tunnels, while the sweep is still running. CORE.
 * @details     A sweep finds addresses in about 20 s; asking each device who it is takes 1-7 s more, so doing
 *              that afterwards and one at a time would dominate the wall clock. Both problems disappear at
 *              once by starting the questions as soon as an address is known and asking several devices in
 *              parallel — each over its own tunnel.
 *
 *              Nothing here reads a device itself. `oknx <pa> info -q` already emits every field as a
 *              tab-separated key/value protocol, and the tool already runs itself as a child process;
 *              a reader is just those two put together. That keeps one implementation
 *              of the read, and a child process cannot disturb the sweep's own tunnel or client state.
 *
 *              How many workers is not a decision worth agonising over: an interface grants what it grants.
 *              A worker whose tunnel is refused simply drops out, the rest carry the queue, and with a
 *              single tunnel left the whole thing still completes — only slower.
 * @date        2026-08-12
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
    #define FTC_SD_POPEN _popen
    #define FTC_SD_PCLOSE _pclose
#else
    #define FTC_SD_POPEN popen
    #define FTC_SD_PCLOSE pclose
#endif

namespace ftc
{

/** @brief What one device answered about itself. Empty strings mean "did not answer that one". */
struct DetailRow
{
    std::string order;   ///< order number / product code
    std::string version; ///< application version, as the device states it
    std::string ftm;     ///< file-transfer module version (empty for a device without it)
    std::string serial;  ///< KNX serial number
    uint16_t mfr = 0;    ///< manufacturer id (0x00FA = OpenKNX)
    uint16_t mask = 0;   ///< mask version the device itself reported (0 = not read)
    // BCU1/BCU2/System 7 identity. Those families have no order number and no KNX serial -- their
    // identity is the manufacturer code plus the 6-digit application number, so without these a BCU
    // answers in full and the row still shows nothing.
    std::string bcuApp;  ///< 6-digit application number, BCD as printed (last pair = version)
    uint8_t bcuMfr = 0;  ///< BCU manufacturer code (0x01 = Siemens)
    uint8_t pei = 0xFF;  ///< PEI type (0xFF = not read)
    uint8_t runState = 0xFF;
    // Load state of the application program, as a number. A device with interface objects reports this
    // instead of a BCU run state. The number, not the name: the child's `app_state` is translated.
    uint8_t appState = 0xFF;
    bool progMode = false;
    bool answered = false;
};

/**
 * @brief A pool of child processes, each with its own tunnel, answering "who are you?" for queued addresses.
 * @details Submit addresses at any time, including while the sweep still runs. `wait()` blocks until the
 *          queue is empty; `rows()` is safe to read at any point and reflects what has come back so far.
 */
class DetailPool
{
  public:
    DetailPool(std::string selfPath, std::string ip, uint16_t port, int workers)
        : _self(std::move(selfPath)), _ip(std::move(ip)), _port(port)
    {
        if (workers < 1) workers = 1;
        for (int i = 0; i < workers; ++i)
            _threads.emplace_back([this]() { run(); });
    }

    ~DetailPool() { wait(); }

    DetailPool(const DetailPool&) = delete;
    DetailPool& operator=(const DetailPool&) = delete;

    /**
     * @brief Queue one address. Ignored once the pool is closing, and never queued twice.
     * @details Every address gets the same budget. An earlier version asked an "acknowledged only"
     *          address once and never again, on the assumption that a device silent to the sweep stays
     *          silent here. That is false: the sweep asks CONNECTIONLESS, this read falls back to a
     *          connection, and BCU1/BCU2 answer only the latter (06 Profiles 4.3 p.42). Measured on
     *          1.1.73 and 1.1.20 — "acknowledged only" in the sweep, full identity over the connection.
     */
    void submit(const std::string& pa, bool = true)
    {
        std::lock_guard<std::mutex> lk(_mtx);
        if (_closing) return;
        if (_seen.count(pa)) return;
        _seen.insert(pa);
        _queue.push_back(Item{pa, 0});
    }

    /** @brief How many addresses are still queued or in flight. */
    size_t outstanding() const
    {
        std::lock_guard<std::mutex> lk(_mtx);
        return _queue.size() + (size_t)_busy.load();
    }

    /** @brief How many answers have come back. */
    size_t answered() const
    {
        std::lock_guard<std::mutex> lk(_mtx);
        return _rows.size();
    }

    /** @brief Let the workers drain the queue and stop. Safe to call more than once. */
    void wait()
    {
        {
            std::lock_guard<std::mutex> lk(_mtx);
            _closing = true;
        }
        for (auto& t : _threads)
            if (t.joinable()) t.join();
        _threads.clear();
    }

    /** @brief Everything answered so far, keyed by address. */
    std::unordered_map<std::string, DetailRow> rows() const
    {
        std::lock_guard<std::mutex> lk(_mtx);
        return _rows;
    }

    /** @brief Ask the workers to stop as soon as they finish what they hold (Ctrl-C). */
    void abort()
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _closing = true;
        _queue.clear();
    }

  private:
    void run()
    {
        for (;;)
        {
            Item it;
            {
                std::lock_guard<std::mutex> lk(_mtx);
                if (!_queue.empty())
                {
                    it = _queue.front();
                    _queue.pop_front();
                }
                else if (_closing)
                    return;
            }
            if (it.pa.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            _busy++;
            bool reached = false;
            DetailRow r = ask(it.pa, reached);
            _busy--;
            // Every address that was reached but told us nothing gets one more go: on a busy line a
            // single read times out while the device is perfectly reachable, and the first attempt falls
            // into the heaviest part of the run. The cost is one extra timeout for an address that truly
            // never answers; the gain is the identity of every BCU on the line.
            const bool worthAnotherGo = !reached || (!r.answered && it.tries == 0);
            if (worthAnotherGo && it.tries + 1 < MAX_TRIES)
            {
                // The child never got as far as talking to the device: every tunnel was busy. That is a
                // statement about the interface, not about the device — put it back rather than record a
                // silence the device never uttered.
                std::lock_guard<std::mutex> lk(_mtx);
                it.tries++;
                _queue.push_back(it);
                continue;
            }
            std::lock_guard<std::mutex> lk(_mtx);
            _rows[it.pa] = r;
        }
    }

    /**
     * @brief Run one child and parse its key/value protocol.
     * @param reached set when the child got a tunnel and reported on the device at all — the difference
     *        between "the device stayed silent" and "we never got to ask".
     */
    DetailRow ask(const std::string& pa, bool& reached) const
    {
        DetailRow r;
        reached = false;
        std::string cmd = "\"" + _self + "\" -i " + _ip + " --port " + std::to_string(_port) + " -q " + pa + " info";
#ifdef _WIN32
        cmd += " 2>NUL";
#else
        cmd += " 2>/dev/null";
#endif
        FILE* f = FTC_SD_POPEN(cmd.c_str(), "r");
        if (!f) return r;
        char line[512];
        while (std::fgets(line, sizeof(line), f))
        {
            char* tab = std::strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            std::string key(line), val(tab + 1);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r')) val.pop_back();
            reached = true; // any key means a tunnel was granted and the device was addressed
            if (key == "order") r.order = val;
            else if (key == "version") r.version = val;
            else if (key == "ftm_version") r.ftm = val;
            else if (key == "serial") r.serial = val;
            else if (key == "manufacturer") r.mfr = (uint16_t)std::strtoul(val.c_str(), nullptr, 0);
            // Not folded into `answered`: a descriptor alone is not an identity, and the one retry a
            // half-answer gets is worth keeping.
            else if (key == "mask") r.mask = (uint16_t)std::strtoul(val.c_str(), nullptr, 0);
            else if (key == "bcu_app") r.bcuApp = val;
            else if (key == "bcu_manufacturer") r.bcuMfr = (uint8_t)std::strtoul(val.c_str(), nullptr, 0);
            else if (key == "bcu_pei") r.pei = (uint8_t)std::strtoul(val.c_str(), nullptr, 0);
            else if (key == "bcu_runstate") r.runState = (uint8_t)std::strtoul(val.c_str(), nullptr, 0);
            else if (key == "app_state_id") r.appState = (uint8_t)std::strtoul(val.c_str(), nullptr, 0);
            else if (key == "progmode") r.progMode = val == "1";
        }
        FTC_SD_PCLOSE(f);
        // A BCU that gave its application number HAS answered, even with no order number and no serial.
        r.answered = !r.order.empty() || !r.serial.empty() || r.mfr != 0 || !r.bcuApp.empty();
        return r;
    }

    /** @brief One queued address plus what we know about it. */
    struct Item
    {
        std::string pa;
        int tries = 0;
    };
    static constexpr int MAX_TRIES = 3; // only ever spent on "no tunnel was free", never on a silent device

    std::string _self, _ip;
    uint16_t _port;
    mutable std::mutex _mtx;
    std::deque<Item> _queue;
    std::unordered_map<std::string, DetailRow> _rows;
    std::set<std::string> _seen;
    std::vector<std::thread> _threads;
    std::atomic<int> _busy{0};
    bool _closing = false;
};

/** @brief 0x00FA — the OpenKNX manufacturer id, as it comes back from a device's serial number. */
constexpr uint16_t MFR_OPENKNX = 0x00FA;

/**
 * @brief The order-info property as a product code, or empty when it does not hold one.
 * @details Many manufacturers store something binary there. The device layer renders unprintable bytes as
 *          dots, so a value that is mostly dots or spaces is padding, not a name — showing it would put
 *          line noise where the user expects a product.
 */
inline std::string orderText(const std::string& v)
{
    size_t filler = 0;
    for (unsigned char ch : v)
        if (ch < 0x20 || ch > 0x7E || ch == '.' || ch == ' ') filler++;
    if (v.empty() || filler * 3 >= v.size()) return std::string();
    return v;
}

/** @brief One readable line about a device: what it is, which version, which file-transfer module. */
inline std::string describe(const DetailRow& r)
{
    std::string out;
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!out.empty()) out += " · ";
        out += part;
    };
    add(orderText(r.order));
    // The device states its version as "[rev] major.minor". The revision matters to an update, not to
    // someone picking a device off a list, so the one-liner carries the part they recognise.
    std::string ver = r.version;
    const size_t close = ver.find(']');
    if (!ver.empty() && ver[0] == '[' && close != std::string::npos)
        ver = ver.substr(close + 1);
    while (!ver.empty() && ver.front() == ' ') ver.erase(ver.begin());
    add(ver.empty() ? std::string() : "Version " + ver);
    add(r.ftm.empty() ? std::string() : "FTM " + r.ftm);
    return out;
}

} // namespace ftc
