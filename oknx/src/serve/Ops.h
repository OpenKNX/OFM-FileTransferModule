/**
 * @file        Ops.h
 * @brief       One description per operation of `oknx serve`: the single source for the MCP tool, the API route,
 *              the input check, the help rows and doc/reference/serve-operations.md.
 * @details     The input schema is plain JSON Schema (what MCP clients read). The check here covers the subset the
 *              descriptions use: type, properties, required, additionalProperties false, enum, minimum, maximum,
 *              minLength, maxLength, minItems, maxItems, items, and `format` for ipv4, knx-pa, knx-ga-selector.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "third_party/json.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace ftc
{
namespace serve
{

using json = nlohmann::json;

/** @brief What an operation does on the network; the rules allow classes per interface. */
enum class OpClass
{
    Passive,     ///< no network traffic
    Net,         ///< multicast search
    DeviceHttp,  ///< GET to the firmware
    BusRead,     ///< sends read telegrams
    GroupMon,    ///< link-layer tunnel for a group monitor
    BusMon,      ///< busmonitor tunnel
    BusWrite,    ///< sends write telegrams
    DeviceWrite, ///< POST to the firmware
};

inline const char* opClassName(OpClass c)
{
    switch (c)
    {
        case OpClass::Passive: return "passive";
        case OpClass::Net: return "net";
        case OpClass::DeviceHttp: return "device-http";
        case OpClass::BusRead: return "bus-read";
        case OpClass::GroupMon: return "group-monitor";
        case OpClass::BusMon: return "busmonitor";
        case OpClass::BusWrite: return "bus-write";
        case OpClass::DeviceWrite: return "device-write";
    }
    return "unknown";
}

inline bool opClassFromName(const std::string& s, OpClass& c)
{
    static const OpClass all[] = {OpClass::Passive, OpClass::Net,    OpClass::DeviceHttp, OpClass::BusRead,
                                  OpClass::GroupMon, OpClass::BusMon, OpClass::BusWrite,   OpClass::DeviceWrite};
    for (OpClass k : all)
        if (s == opClassName(k))
        {
            c = k;
            return true;
        }
    return false;
}

/** @brief Where an operation runs: in the leader (no KNXnet/IP socket) or in the interface's worker. */
enum class OpWhere
{
    Leader,
    Worker
};

struct OpDesc
{
    std::string name;
    std::string titleEn, titleDe;
    std::string helpEn, helpDe;
    OpClass cls = OpClass::Passive; ///< for monitor/compare/monitor_stop the class follows `mode` (classFor)
    OpWhere where = OpWhere::Leader;
    json input;                     ///< JSON Schema of the arguments
    json output;                    ///< JSON Schema of `data` (MCP outputSchema)
    uint32_t timeoutMs = 30000;     ///< hard limit serve enforces
    bool progress = false;          ///< reports progress
    std::vector<std::string> secret; ///< argument names never logged or echoed
    bool changes = false;           ///< changes something on the bus or the device
};

/** @brief The class an operation takes for these arguments (the monitors follow their mode). */
inline OpClass classFor(const OpDesc& d, const json& args)
{
    if (d.name == "monitor" || d.name == "compare" || d.name == "monitor_stop")
        return args.value("mode", std::string("gm")) == "bm" ? OpClass::BusMon : OpClass::GroupMon;
    if (d.name == "runstate" && args.value("action", std::string("read")) == "read") return OpClass::BusRead;
    return d.cls;
}

namespace detail_ops
{
inline json obj(json props, std::vector<std::string> required)
{
    return json{{"type", "object"}, {"properties", std::move(props)}, {"required", required}, {"additionalProperties", false}};
}
inline json str(const std::string& desc, const std::string& format = std::string())
{
    json j = {{"type", "string"}, {"description", desc}};
    if (!format.empty()) j["format"] = format;
    return j;
}
inline json integer(const std::string& desc, long long lo, long long hi)
{
    return json{{"type", "integer"}, {"description", desc}, {"minimum", lo}, {"maximum", hi}};
}
inline json ipField() { return str("IPv4 address of the KNXnet/IP interface", "ipv4"); }
} // namespace detail_ops

/** @brief Every operation serve offers. */
inline const std::vector<OpDesc>& opTable()
{
    using namespace detail_ops;
    static const std::vector<OpDesc> t = [] {
        std::vector<OpDesc> v;
        {
            OpDesc d;
            d.name = "discover";
            d.titleEn = "Search for KNXnet/IP interfaces";
            d.titleDe = "KNXnet/IP-Interfaces suchen";
            d.helpEn = "Multicast SEARCH and mDNS on the local network; lists every interface that answers.";
            d.helpDe = "Multicast-SEARCH und mDNS im lokalen Netz; listet jedes Interface, das antwortet.";
            d.cls = OpClass::Net;
            d.input = obj({{"timeout_ms", integer("how long to listen", 500, 5000)}}, {});
            d.output = json{{"type", "object"}, {"properties", {{"interfaces", {{"type", "array"}}}}}};
            d.timeoutMs = 8000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_state";
            d.titleEn = "Interface state from its firmware";
            d.titleDe = "Zustand eines Interfaces aus seiner Firmware";
            d.helpEn = "GET /ipif/state or /ipro/state of an OpenKNX interface: tunnels, busmonitor, owner. No tunnel slot.";
            d.helpDe = "GET /ipif/state bzw. /ipro/state eines OpenKNX-Interfaces: Tunnel, Busmonitor, Besitzer. Kein Tunnel-Platz.";
            d.cls = OpClass::DeviceHttp;
            d.input = obj({{"ip", ipField()}}, {"ip"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_bus";
            d.titleEn = "Bus coupler figures from the firmware";
            d.titleDe = "Buskoppler-Werte aus der Firmware";
            d.helpEn = "GET /ipif/bus or /ipro/bus: the figures of `bcu stat`.";
            d.helpDe = "GET /ipif/bus bzw. /ipro/bus: die Werte von `bcu stat`.";
            d.cls = OpClass::DeviceHttp;
            d.input = obj({{"ip", ipField()}}, {"ip"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "tunnels";
            d.titleEn = "Tunnel slots of an interface";
            d.titleDe = "Tunnel-Plätze eines Interfaces";
            d.helpEn = "Which slots are taken and by whom, from the firmware, else from serve's own book. Never takes a slot to count.";
            d.helpDe = "Welche Plätze belegt sind und von wem, aus der Firmware, sonst aus serves Buchführung. Belegt nie einen Platz zum Zählen.";
            d.cls = OpClass::DeviceHttp;
            d.input = obj({{"ip", ipField()}}, {"ip"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_info";
            d.titleEn = "Identity of a device";
            d.titleDe = "Kennung eines Geräts";
            d.helpEn = "Mask, manufacturer, order number, version, serial number, programming mode and FTC support; without pa the interface itself.";
            d.helpDe = "Maske, Hersteller, Bestellnummer, Version, Seriennummer, Programmiermodus und FTC; ohne pa das Interface selbst.";
            d.cls = OpClass::BusRead;
            d.where = OpWhere::Worker;
            d.input = obj({{"ip", ipField()}, {"pa", str("individual address of the device", "knx-pa")}}, {"ip"});
            d.timeoutMs = 30000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "ga_read";
            d.titleEn = "Read group addresses";
            d.titleDe = "Gruppenadressen lesen";
            d.helpEn = "A_GroupValue_Read per address, as `oknx ga read`: x/y/z, x/y/z-n, x/y/*, x/*, * over the imported table; x/y/z:DPT sets the type.";
            d.helpDe = "A_GroupValue_Read je Adresse wie `oknx ga read`: x/y/z, x/y/z-n, x/y/*, x/*, * über die importierte Tabelle; x/y/z:DPT setzt den Typ.";
            d.cls = OpClass::BusRead;
            d.where = OpWhere::Worker;
            d.input = obj({{"ip", ipField()},
                           {"ga", json{{"type", "array"},
                                       {"minItems", 1},
                                       {"maxItems", 256},
                                       {"items", str("group address selector", "knx-ga-selector")}}}},
                          {"ip", "ga"});
            d.timeoutMs = 120000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "prop_read";
            d.titleEn = "Read a property of the interface";
            d.titleDe = "Eine Property des Interfaces lesen";
            d.helpEn = "Device management M_PropRead on the interface itself (as `oknx prop read`).";
            d.helpDe = "Device Management M_PropRead am Interface selbst (wie `oknx prop read`).";
            d.cls = OpClass::BusRead;
            d.where = OpWhere::Worker;
            d.input = obj({{"ip", ipField()},
                           {"object_type", integer("interface object type", 0, 65535)},
                           {"instance", integer("object instance", 1, 255)},
                           {"pid", integer("property id", 0, 255)},
                           {"start", integer("start index", 0, 4095)},
                           {"count", integer("number of elements", 0, 15)}},
                          {"ip", "object_type", "instance", "pid"});
            d.timeoutMs = 10000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "busprop_read";
            d.titleEn = "Read a property of a bus device";
            d.titleDe = "Eine Property eines Busgeräts lesen";
            d.helpEn = "A_PropertyValue_Read to a device on the bus (as `oknx busprop read`).";
            d.helpDe = "A_PropertyValue_Read an ein Gerät am Bus (wie `oknx busprop read`).";
            d.cls = OpClass::BusRead;
            d.where = OpWhere::Worker;
            d.input = obj({{"ip", ipField()},
                           {"pa", str("individual address of the device", "knx-pa")},
                           {"object_index", integer("object index", 0, 255)},
                           {"pid", integer("property id", 0, 255)},
                           {"start", integer("start index", 0, 4095)},
                           {"count", integer("number of elements", 1, 15)}},
                          {"ip", "pa", "object_index", "pid"});
            d.timeoutMs = 10000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "monitor";
            d.titleEn = "Watch the bus for a while";
            d.titleDe = "Den Bus eine Zeit lang beobachten";
            d.helpEn = "Group monitor (gm) or busmonitor (bm), shared with every other reader; returns statistics, up to max_frames telegrams and the full capture as a file.";
            d.helpDe = "Gruppenmonitor (gm) oder Busmonitor (bm), geteilt mit allen anderen Lesern; liefert Statistik, bis zu max_frames Telegramme und die volle Aufzeichnung als Datei.";
            d.cls = OpClass::GroupMon;
            d.input = obj({{"ip", ipField()},
                           {"mode", json{{"type", "string"}, {"enum", {"gm", "bm"}}, {"description", "gm = group monitor, bm = busmonitor"}}},
                           {"seconds", integer("how long", 1, 300)},
                           {"filter_ga", json{{"type", "array"}, {"maxItems", 256}, {"items", str("group address selector", "knx-ga-selector")}}},
                           {"max_frames", integer("telegrams in the result", 0, 500)}},
                          {"ip", "mode", "seconds"});
            d.timeoutMs = 330000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "compare";
            d.titleEn = "Compare two interfaces on one line";
            d.titleDe = "Zwei Interfaces an einer Linie vergleichen";
            d.helpEn = "Group monitor or busmonitor on ip and ref at once; the counters of `oknx gm|bm compare -q`.";
            d.helpDe = "Gruppenmonitor oder Busmonitor an ip und ref gleichzeitig; die Zähler von `oknx gm|bm compare -q`.";
            d.cls = OpClass::GroupMon;
            d.input = obj({{"ip", ipField()},
                           {"ref", str("IPv4 address of the reference interface", "ipv4")},
                           {"ref_port", integer("KNXnet/IP port of the reference (default: port)", 1, 65535)},
                           {"mode", json{{"type", "string"}, {"enum", {"gm", "bm"}}}},
                           {"seconds", integer("how long", 1, 300)},
                           {"grace_ms", integer("match window", 50, 10000)}},
                          {"ip", "ref", "mode", "seconds"});
            d.timeoutMs = 330000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "bench_state";
            d.titleEn = "What serve holds and what the bench lock says";
            d.titleDe = "Was serve hält und was die Belegungsdatei sagt";
            d.helpEn = "Connections (who, since when, readers), reservations, the suite's lock file, running and finished operations.";
            d.helpDe = "Verbindungen (wer, seit wann, Leser), Reservierungen, die Belegungsdatei der Suite, laufende und beendete Operationen.";
            d.cls = OpClass::Passive;
            d.input = obj(json::object(), {});
            d.timeoutMs = 2000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "monitor_stop";
            d.titleEn = "End a shared monitor";
            d.titleDe = "Einen geteilten Monitor beenden";
            d.helpEn = "Ends the monitor with all its readers (they get CANCELLED); frees the interface for management operations.";
            d.helpDe = "Beendet den Monitor samt Lesern (die bekommen CANCELLED); gibt das Interface für Management-Operationen frei.";
            d.cls = OpClass::GroupMon;
            d.input = obj({{"ip", ipField()}, {"mode", json{{"type", "string"}, {"enum", {"gm", "bm"}}}}}, {"ip", "mode"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        // --- the writing operations: each names what it changes --------------------------
        const json item = json{{"type", "object"},
                               {"properties",
                                {{"ga", str("one group address x/y/z, optionally :DPT (x/y/z:9.001)", "knx-ga-selector")},
                                 {"value", str("the value as on the command line: 21.5, 1, a text, or hex octets 0C 1A")}}},
                               {"required", {"ga", "value"}},
                               {"additionalProperties", false}};
        {
            OpDesc d;
            d.name = "ga_write";
            d.titleEn = "Write group addresses";
            d.titleDe = "Gruppenadressen schreiben";
            d.helpEn = "A_GroupValue_Write per item, as `oknx ga write`: the imported type (or :DPT) encodes the value; untyped 0-63 or hex octets. Changes the bus.";
            d.helpDe = "A_GroupValue_Write je Eintrag wie `oknx ga write`: der importierte Typ (oder :DPT) kodiert den Wert; ohne Typ 0-63 oder Hex-Oktette. Ändert den Bus.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()}, {"items", json{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", item}}}},
                          {"ip", "items"});
            d.progress = true; // long enough to need a stream and progress over MCP
            d.timeoutMs = 300000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "ga_ask";
            d.titleEn = "Send a command on group addresses and collect the answers";
            d.titleDe = "Ein Kommando auf Gruppenadressen senden und die Antworten sammeln";
            d.helpEn = "As `oknx ga ask`: writes each item, then collects GroupValue_Write/Response on the same address until all answered (300 ms after the last), at most wait_ms. Changes the bus.";
            d.helpDe = "Wie `oknx ga ask`: schreibt jeden Eintrag, sammelt dann GroupValue_Write/Response auf derselben Adresse, bis alle geantwortet haben (300 ms nach der letzten), höchstens wait_ms. Ändert den Bus.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()},
                           {"items", json{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", item}}},
                           {"wait_ms", integer("longest wait for answers after the last telegram", 100, 60000)}},
                          {"ip", "items"});
            d.timeoutMs = 300000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "restart";
            d.titleEn = "Restart a device";
            d.titleDe = "Ein Gerät neu starten";
            d.helpEn = "A_Restart over a transport connection (the mandatory form), as `oknx <pa> restart`. The device reboots; nothing answers.";
            d.helpDe = "A_Restart über eine Transportverbindung (die Pflichtform), wie `oknx <pa> restart`. Das Gerät startet neu; es antwortet nichts.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()}, {"pa", str("individual address of the device", "knx-pa")}}, {"ip", "pa"});
            d.timeoutMs = 15000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "runstate";
            d.titleEn = "Run state of the application program";
            d.titleDe = "Laufzustand des Applikationsprogramms";
            d.helpEn = "PID_RUN_STATE_CONTROL of the application program object, as `oknx <pa> runstate`: read, or start / stop (in RAM; a restart undoes a stop). Stop and start change the device.";
            d.helpDe = "PID_RUN_STATE_CONTROL des Applikationsobjekts wie `oknx <pa> runstate`: lesen, oder start / stop (im RAM; ein Neustart hebt stop auf). Stop und start ändern das Gerät.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()},
                           {"pa", str("individual address of the device", "knx-pa")},
                           {"action", json{{"type", "string"}, {"enum", {"read", "start", "stop"}}}}},
                          {"ip", "pa"});
            d.timeoutMs = 30000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "setpa";
            d.titleEn = "Give a device its individual address";
            d.titleDe = "Einem Gerät seine physikalische Adresse geben";
            d.helpEn = "As `oknx setpa`: with serial, A_IndividualAddressSerialNumber_Write to that device; without, the one device in programming mode (waits up to wait_s for exactly one). Checks the address is free, reads it back, restarts the device so it keeps it (restart:false leaves that out on the serial-number way; in programming mode the restart is what ends it and is always sent). Several devices in programming mode: BUSY with their serial numbers. Changes the device.";
            d.helpDe = "Wie `oknx setpa`: mit serial A_IndividualAddressSerialNumber_Write an dieses Gerät; ohne an das eine Gerät im Programmiermodus (wartet bis zu wait_s auf genau eins). Prüft, dass die Adresse frei ist, liest zurück, startet das Gerät neu, damit es sie behält (restart:false lässt das beim Weg über die Seriennummer weg; im Programmiermodus beendet erst der Neustart ihn und wird immer gesendet). Mehrere Geräte im Programmiermodus: BUSY mit ihren Seriennummern. Ändert das Gerät.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()},
                           {"pa", str("the new individual address", "knx-pa")},
                           {"serial", str("serial number of the device, e.g. 00FA:01A2B3C4")},
                           {"wait_s", integer("how long to wait for exactly one device in programming mode", 0, 120)},
                           {"restart", json{{"type", "boolean"}, {"description", "restart afterwards so the device keeps the address (default true; serial-number way only)"}}}},
                          {"ip", "pa"});
            d.progress = true; // long enough to need a stream and progress over MCP
            d.timeoutMs = 180000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "ftc_login";
            d.titleEn = "Sign in to a password-protected FTC device";
            d.titleDe = "An einem passwortgeschützten FTC-Gerät anmelden";
            d.helpEn = "The FTC challenge-response, as `oknx <pa> login`. The password never travels over MCP or the API: serve takes it from OKNX_FTC_PW_<a>_<l>_<d> of its own environment, or asks in its own terminal. Opens the device's write window until it goes idle.";
            d.helpDe = "Das FTC-Challenge-Response wie `oknx <pa> login`. Das Passwort läuft nie über MCP oder die API: serve nimmt es aus OKNX_FTC_PW_<a>_<l>_<d> seiner eigenen Umgebung oder fragt in seinem eigenen Terminal. Öffnet das Schreibfenster des Geräts, bis es in Ruhe fällt.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.secret = {"password"};
            d.input = obj({{"ip", ipField()}, {"pa", str("individual address of the device", "knx-pa")}}, {"ip", "pa"});
            d.progress = true; // long enough to need a stream and progress over MCP
            d.timeoutMs = 150000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "ftc_send";
            d.titleEn = "Upload a file to a device (FTC)";
            d.titleDe = "Eine Datei auf ein Gerät laden (FTC)";
            d.helpEn = "As `oknx <pa> send <src> <dst> [safe|fast]`; src is a file on the machine serve runs on. A transfer stopped by a crash leaves the file open on the device for up to 30 s (no heartbeat), a new one is refused (0x41) until then. Changes the device.";
            d.helpDe = "Wie `oknx <pa> send <src> <dst> [safe|fast]`; src ist eine Datei auf dem Rechner, auf dem serve läuft. Eine durch einen Absturz abgebrochene Übertragung lässt die Datei auf dem Gerät bis zu 30 s offen (kein Heartbeat), eine neue wird bis dahin abgelehnt (0x41). Ändert das Gerät.";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()},
                           {"pa", str("individual address of the device", "knx-pa")},
                           {"src", str("local file path (no spaces)")},
                           {"dst", str("path on the device, optionally sd/ or efc/ (no spaces)")},
                           {"mode", json{{"type", "string"}, {"enum", {"safe", "fast"}}}}},
                          {"ip", "pa", "src", "dst"});
            d.timeoutMs = 3600000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "ftc_get";
            d.titleEn = "Fetch a file from a device (FTC)";
            d.titleDe = "Eine Datei von einem Gerät holen (FTC)";
            d.helpEn = "As `oknx <pa> get <src> <dst>`; dst is a file on the machine serve runs on (it is written).";
            d.helpDe = "Wie `oknx <pa> get <src> <dst>`; dst ist eine Datei auf dem Rechner, auf dem serve läuft (sie wird geschrieben).";
            d.cls = OpClass::BusWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()},
                           {"pa", str("individual address of the device", "knx-pa")},
                           {"src", str("path on the device (no spaces)")},
                           {"dst", str("local file path (no spaces)")}},
                          {"ip", "pa", "src", "dst"});
            d.timeoutMs = 3600000;
            d.progress = true;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_tunnel_close";
            d.titleEn = "Close a tunnel connection on an OpenKNX interface";
            d.titleDe = "Eine Tunnelverbindung an einem OpenKNX-Interface trennen";
            d.helpEn = "POST /ipif/close?ch=<ch> (or /ipro/close): the interface ends that connection, whoever holds it. Changes the device.";
            d.helpDe = "POST /ipif/close?ch=<ch> (bzw. /ipro/close): das Interface beendet diese Verbindung, egal wem sie gehört. Ändert das Gerät.";
            d.cls = OpClass::DeviceWrite;
            d.changes = true;
            d.input = obj({{"ip", ipField()}, {"ch", integer("KNXnet/IP channel id (from tunnels)", 1, 255)}}, {"ip", "ch"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_busmon_stop";
            d.titleEn = "End the busmonitor on an OpenKNX interface";
            d.titleDe = "Den Busmonitor an einem OpenKNX-Interface beenden";
            d.helpEn = "POST /ipif/busmon/stop: ends the busmonitor whoever holds it (tunnel or console). Changes the device.";
            d.helpDe = "POST /ipif/busmon/stop: beendet den Busmonitor, egal wer ihn hält (Tunnel oder Konsole). Ändert das Gerät.";
            d.cls = OpClass::DeviceWrite;
            d.changes = true;
            d.input = obj({{"ip", ipField()}}, {"ip"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_reset";
            d.titleEn = "Reset the counters of an OpenKNX interface";
            d.titleDe = "Die Zähler eines OpenKNX-Interfaces zurücksetzen";
            d.helpEn = "POST /ipif/reset (or /ipro/reset), as the console's `ipif reset`: bus load and lost-telegram counters start again. Not a restart.";
            d.helpDe = "POST /ipif/reset (bzw. /ipro/reset) wie `ipif reset` in der Konsole: Buslast- und Verlustzähler beginnen neu. Kein Neustart.";
            d.cls = OpClass::DeviceWrite;
            d.changes = true;
            d.input = obj({{"ip", ipField()}}, {"ip"});
            d.timeoutMs = 5000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "device_mreset";
            d.titleEn = "Restart an interface";
            d.titleDe = "Ein Interface neu starten";
            d.helpEn = "M_Reset.req over device management, as `oknx -i <ip> reset`: the interface does the steps of a Basic Restart (an OpenKNX interface does not reboot); every KNXnet/IP connection to it closes, also those of other clients. Waits up to wait_s for it to answer again.";
            d.helpDe = "M_Reset.req über Device Management wie `oknx -i <ip> reset`: das Interface führt die Schritte eines Basic Restart aus (ein OpenKNX-Interface bootet dabei nicht neu); jede KNXnet/IP-Verbindung zu ihm schließt, auch die anderer Clients. Wartet bis zu wait_s, bis es wieder antwortet.";
            d.cls = OpClass::DeviceWrite;
            d.where = OpWhere::Worker;
            d.changes = true;
            d.input = obj({{"ip", ipField()}, {"wait_s", integer("how long to wait for the interface to answer again (default 30)", 1, 120)}}, {"ip"});
            d.timeoutMs = 150000;
            v.push_back(d);
        }
        {
            OpDesc d;
            d.name = "suite_run";
            d.titleEn = "Start a test suite run";
            d.titleDe = "Einen Lauf der Test-Suite starten";
            d.helpEn = "Run-Tests.ps1 -TestProfile <profile> next to the bench config. The run holds the bench through its lock file; follow it with bench_state. Needs \"suite_run\": true in a bench site of the rules. Changes the bench devices.";
            d.helpDe = "Run-Tests.ps1 -TestProfile <profile> neben der Prüfstand-Konfiguration. Der Lauf belegt den Prüfstand über seine Belegungsdatei; verfolgen mit bench_state. Braucht \"suite_run\": true in einem Prüfstand-Abschnitt der Regeln. Ändert die Prüfstand-Geräte.";
            d.cls = OpClass::BusWrite;
            d.changes = true;
            d.input = obj({{"profile", str("profile name from scripts/Test/Profiles, e.g. quick-check")}}, {"profile"});
            d.timeoutMs = 15000;
            v.push_back(d);
        }

        // every operation on an interface also takes its KNXnet/IP port (default 3671)
        for (auto& d : v)
            if (d.input.contains("properties") && d.input["properties"].contains("ip"))
                d.input["properties"]["port"] = integer("KNXnet/IP port of the interface (default 3671)", 1, 65535);
        return v;
    }();
    return t;
}

inline const OpDesc* findOp(const std::string& name)
{
    for (const auto& d : opTable())
        if (d.name == name) return &d;
    return nullptr;
}

/**********************************************************************
 ***************************** CHECKING ******************************
 **********************************************************************/
inline bool isIpv4(const std::string& s)
{
    unsigned a, b, c, d;
    char tail = 0;
    return std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a <= 255 && b <= 255 && c <= 255 &&
           d <= 255 && s.find_first_not_of("0123456789.") == std::string::npos;
}

inline bool isKnxPa(const std::string& s)
{
    unsigned a, l, d;
    char tail = 0;
    return std::sscanf(s.c_str(), "%u.%u.%u%c", &a, &l, &d, &tail) == 3 && a <= 15 && l <= 15 && d <= 255 &&
           s.find_first_not_of("0123456789.") == std::string::npos;
}

/** @brief Shape check of a group address selector; the full meaning is checked where it is expanded. */
inline bool isGaSelector(const std::string& s)
{
    if (s.empty() || s.size() > 32) return false;
    return s.find_first_not_of("0123456789/*-:.") == std::string::npos;
}

/**
 * @brief Check @p v against @p schema; on failure fill @p where (a JSON pointer) and @p why, and return false.
 */
inline bool checkSchema(const json& schema, const json& v, const std::string& path, std::string& where, std::string& why)
{
    auto bad = [&](const std::string& w) {
        where = path.empty() ? "/" : path;
        why = w;
        return false;
    };
    const std::string type = schema.value("type", std::string());
    if (type == "object")
    {
        if (!v.is_object()) return bad("must be an object");
        const json props = schema.value("properties", json::object());
        for (const auto& r : schema.value("required", json::array()))
            if (!v.contains(r.get<std::string>())) return bad("missing " + r.get<std::string>());
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            if (!props.contains(it.key()))
            {
                if (schema.value("additionalProperties", true) == false) return bad("unknown argument " + it.key());
                continue;
            }
            if (!checkSchema(props[it.key()], it.value(), path + "/" + it.key(), where, why)) return false;
        }
        return true;
    }
    if (type == "array")
    {
        if (!v.is_array()) return bad("must be an array");
        if (schema.contains("minItems") && v.size() < schema["minItems"].get<size_t>()) return bad("too few entries");
        if (schema.contains("maxItems") && v.size() > schema["maxItems"].get<size_t>()) return bad("too many entries");
        if (schema.contains("items"))
            for (size_t i = 0; i < v.size(); ++i)
                if (!checkSchema(schema["items"], v[i], path + "/" + std::to_string(i), where, why)) return false;
        return true;
    }
    if (type == "integer")
    {
        if (!v.is_number_integer()) return bad("must be an integer");
        const long long x = v.get<long long>();
        if (schema.contains("minimum") && x < schema["minimum"].get<long long>()) return bad("below the minimum");
        if (schema.contains("maximum") && x > schema["maximum"].get<long long>()) return bad("above the maximum");
        return true;
    }
    if (type == "number")
    {
        if (!v.is_number()) return bad("must be a number");
        return true;
    }
    if (type == "boolean")
    {
        if (!v.is_boolean()) return bad("must be true or false");
        return true;
    }
    if (type == "string")
    {
        if (!v.is_string()) return bad("must be a string");
        const std::string s = v.get<std::string>();
        if (schema.contains("minLength") && s.size() < schema["minLength"].get<size_t>()) return bad("too short");
        if (schema.contains("maxLength") && s.size() > schema["maxLength"].get<size_t>()) return bad("too long");
        if (schema.contains("enum"))
        {
            bool hit = false;
            for (const auto& e : schema["enum"])
                if (e == v) hit = true;
            if (!hit) return bad("not one of the allowed values");
        }
        const std::string fmt = schema.value("format", std::string());
        if (fmt == "ipv4" && !isIpv4(s)) return bad("not an IPv4 address");
        if (fmt == "knx-pa" && !isKnxPa(s)) return bad("not an individual address a.l.d");
        if (fmt == "knx-ga-selector" && !isGaSelector(s)) return bad("not a group address selector");
        return true;
    }
    return true;
}

} // namespace serve
} // namespace ftc
