/**
 * @file        GaImport.h
 * @brief       Read an ETS group-address export into the GaTable.
 * @details     Covers all four output formats of the ETS "Gruppenadressen exportieren" dialog, measured
 *              against real exports of one project (2026-10-02):
 *
 *                CSV 1/1   "Group name" + "Address"                       + DatapointType
 *                CSV 1/3   "Group name" + Main/Middle/Sub (numeric)       + DatapointType
 *                CSV 3/1   name triple  + "Address"                       + DatapointType
 *                CSV 3/3   name triple  + address triple                  + DatapointType
 *                CSV ETS3  name triple  + "Address"                       - NO datapoint type
 *                XML       Name= Address=                                 + DPTs="DPST-m-s"
 *                XML ETS4  Name= Address=                                 - NO datapoint type
 *
 *              Two of the seven carry no type at all, so the importer counts what it found and says so
 *              rather than storing a table that will decode nothing.
 *              The CSV files are ISO-8859-1 with CRLF and quoted fields; the XML is UTF-8 with a BOM.
 * @date        2026-10-02
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "GaTable.h"

namespace ftc
{

/// @brief What an import found, so the caller can report it instead of claiming success.
struct GaImportResult
{
    bool ok = false;
    size_t addresses = 0; ///< group addresses read
    size_t withDpt = 0;   ///< of those, how many carried a datapoint type
    std::string format;   ///< the format that was recognised, for the report
    std::string error;
};

class GaImport
{
  public:
    /// @brief Read @p file into @p t (which is cleared first - an export is a full picture).
    static GaImportResult run(const std::string& file, GaTable& t)
    {
        GaImportResult r;
        std::ifstream f(file, std::ios::binary);
        if (!f)
        {
            r.error = "cannot open " + file;
            return r;
        }
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.size() >= 3 && (uint8_t)raw[0] == 0xEF && (uint8_t)raw[1] == 0xBB && (uint8_t)raw[2] == 0xBF)
            raw.erase(0, 3); // UTF-8 BOM (the XML exports carry one)

        t.clear();
        if (raw.find("<GroupAddress-Export") != std::string::npos || raw.find("<GroupAddress ") != std::string::npos)
            return xml(raw, t);
        return csv(raw, t);
    }

  private:
    /// @brief ISO-8859-1 -> UTF-8, but only when the text is not valid UTF-8 already. The CSV exports are
    ///        Latin-1, the XML is UTF-8; guessing wrong would mangle every umlaut in a name.
    static std::string toUtf8(const std::string& in)
    {
        if (isUtf8(in)) return in;
        std::string out;
        out.reserve(in.size() + 8);
        for (unsigned char c : in)
        {
            if (c < 0x80) out += (char)c;
            else
            {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    static bool isUtf8(const std::string& s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const unsigned char c = (unsigned char)s[i];
            size_t n = 0;
            if (c < 0x80) n = 0;
            else if ((c & 0xE0) == 0xC0) n = 1;
            else if ((c & 0xF0) == 0xE0) n = 2;
            else if ((c & 0xF8) == 0xF0) n = 3;
            else return false;
            if (i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
            for (size_t k = 1; k <= n; k++)
                if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
            i += n + 1;
        }
        return true;
    }

    static std::string attr(const std::string& tag, const char* name)
    {
        const std::string key = std::string(name) + "=\"";
        const size_t a = tag.find(key);
        if (a == std::string::npos) return "";
        const size_t b = tag.find('"', a + key.size());
        if (b == std::string::npos) return "";
        return tag.substr(a + key.size(), b - a - key.size());
    }

    static GaImportResult xml(const std::string& raw, GaTable& t)
    {
        GaImportResult r;
        r.format = "XML";
        size_t p = 0;
        while ((p = raw.find("<GroupAddress ", p)) != std::string::npos)
        {
            const size_t e = raw.find("/>", p);
            if (e == std::string::npos) break;
            const std::string tag = raw.substr(p, e - p);
            p = e + 2;
            uint16_t ga = 0;
            if (!GaTable::parseGa(attr(tag, "Address"), ga)) continue;
            GaInfo gi;
            gi.name = unescape(attr(tag, "Name"));
            // ETS writes the list attribute "DPTs"; a project may hold several, the first one wins.
            std::string d = attr(tag, "DPTs");
            if (d.empty()) d = attr(tag, "DatapointType");
            const size_t sp = d.find(' ');
            if (sp != std::string::npos) d = d.substr(0, sp);
            if (!d.empty() && GaTable::parseDpt(d, gi.main, gi.sub)) r.withDpt++;
            t.set(ga, gi);
            r.addresses++;
        }
        if (r.addresses == 0) r.error = "no group address in this XML";
        r.ok = r.addresses > 0;
        if (r.ok && r.withDpt == 0) r.format = "XML (ETS4 style, no datapoint types)";
        return r;
    }

    static std::string unescape(const std::string& in)
    {
        std::string o;
        for (size_t i = 0; i < in.size(); i++)
        {
            if (in[i] != '&') { o += in[i]; continue; }
            if (in.compare(i, 5, "&amp;") == 0) { o += '&'; i += 4; }
            else if (in.compare(i, 4, "&lt;") == 0) { o += '<'; i += 3; }
            else if (in.compare(i, 4, "&gt;") == 0) { o += '>'; i += 3; }
            else if (in.compare(i, 6, "&quot;") == 0) { o += '"'; i += 5; }
            else if (in.compare(i, 6, "&apos;") == 0) { o += '\''; i += 5; }
            else o += in[i];
        }
        return o;
    }

    /// @brief Split one CSV line on @p sep, honouring quotes and the doubled "" escape.
    static std::vector<std::string> split(const std::string& line, char sep)
    {
        std::vector<std::string> out;
        std::string cur;
        bool q = false;
        for (size_t i = 0; i < line.size(); i++)
        {
            const char c = line[i];
            if (q)
            {
                if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
                else if (c == '"') q = false;
                else cur += c;
            }
            else if (c == '"') q = true;
            else if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        out.push_back(cur);
        return out;
    }

    static GaImportResult csv(const std::string& rawIn, GaTable& t)
    {
        GaImportResult r;
        const std::string raw = toUtf8(rawIn);
        std::istringstream in(raw);
        std::string line;
        if (!std::getline(in, line))
        {
            r.error = "empty file";
            return r;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        // The separator is whichever of the three the header uses most; ETS offers tab, comma, semicolon.
        char sep = '\t';
        size_t best = 0;
        for (char c : {'\t', ';', ','})
        {
            const size_t n = (size_t)std::count(line.begin(), line.end(), c);
            if (n > best) { best = n; sep = c; }
        }
        const std::vector<std::string> head = split(line, sep);

        // Column roles. Duplicate "Main/Middle/Sub" triples exist (format 3/3): the FIRST triple is the
        // name hierarchy, the LAST one is the address - measured on a real 3/3 export.
        int cName = -1, cAddr = -1, cDpt = -1;
        std::vector<int> mains, mids, subs;
        for (size_t i = 0; i < head.size(); i++)
        {
            const std::string h = lower(head[i]);
            if (h == "group name" || h == "gruppenname") cName = (int)i;
            else if (h == "address" || h == "adresse") cAddr = (int)i;
            else if (h == "datapointtype" || h == "datenpunkttyp") cDpt = (int)i;
            else if (h == "main" || h == "hauptgruppe") mains.push_back((int)i);
            else if (h == "middle" || h == "mittelgruppe") mids.push_back((int)i);
            else if (h == "sub" || h == "untergruppe") subs.push_back((int)i);
        }
        const bool tripleAddr = cAddr < 0 && mains.size() >= 1 && mids.size() >= 1 && subs.size() >= 1;
        if (cAddr < 0 && !tripleAddr)
        {
            // Without a header there is nothing to key the columns on, and reading it positionally would
            // mean guessing which of the four CSV layouts it is - and silently eating the first address
            // as a header on top. An export without header lines is refused, not half understood.
            bool looksLikeData = false;
            for (const std::string& c : head)
            {
                uint16_t probe = 0;
                if (GaTable::parseGa(c, probe)) looksLikeData = true;
            }
            r.error = looksLikeData ? "this CSV has no header line - export it again with \"Export mit Kopfzeilen\""
                                    : "no address column in this CSV (header line not recognised)";
            return r;
        }
        r.format = std::string("CSV, separator ") + (sep == '\t' ? "tab" : sep == ';' ? "semicolon" : "comma");

        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            const std::vector<std::string> f = split(line, sep);
            uint16_t ga = 0;
            if (cAddr >= 0)
            {
                if ((size_t)cAddr >= f.size()) continue;
                if (!GaTable::parseGa(f[cAddr], ga)) continue; // "0/-/-" is a range header, not an address
            }
            else
            {
                const int im = mains.back(), ii = mids.back(), is = subs.back();
                if ((size_t)im >= f.size() || (size_t)ii >= f.size() || (size_t)is >= f.size()) continue;
                if (f[im].empty() || f[ii].empty() || f[is].empty()) continue; // a range row, not a leaf
                if (!GaTable::parseGa(f[im] + "/" + f[ii] + "/" + f[is], ga)) continue;
            }
            GaInfo gi;
            if (cName >= 0 && (size_t)cName < f.size()) gi.name = f[cName];
            else
            {
                // name triple: the deepest filled level is this row's own name
                for (int idx : {subs.empty() ? -1 : subs.front(), mids.empty() ? -1 : mids.front(),
                                mains.empty() ? -1 : mains.front()})
                    if (idx >= 0 && (size_t)idx < f.size() && !f[idx].empty()) { gi.name = f[idx]; break; }
            }
            if (cDpt >= 0 && (size_t)cDpt < f.size() && !f[cDpt].empty())
            {
                std::string d = f[cDpt];
                const size_t sp = d.find(' ');
                if (sp != std::string::npos) d = d.substr(0, sp);
                if (GaTable::parseDpt(d, gi.main, gi.sub)) r.withDpt++;
            }
            t.set(ga, gi);
            r.addresses++;
        }
        if (r.addresses == 0) r.error = "no group address row in this CSV";
        r.ok = r.addresses > 0;
        if (r.ok && cDpt < 0) r.format += " (ETS3 style, no datapoint types)";
        return r;
    }

    static std::string lower(const std::string& s)
    {
        std::string o;
        for (char c : s)
            if (c != '"') o += (char)std::tolower((unsigned char)c);
        return o;
    }
};

} // namespace ftc
