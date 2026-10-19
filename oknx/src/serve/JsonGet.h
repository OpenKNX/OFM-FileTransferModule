/**
 * @file        JsonGet.h
 * @brief       Read one member of a JSON object written by another program without throwing.
 * @details     nlohmann::json::value() throws when the member has another type ("pid": "123"); outside an HTTP
 *              handler that ends serve through std::terminate. jget() returns the default instead.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "third_party/json.hpp"

namespace ftc
{
namespace serve
{

/** @brief @p j[@p key] as T, or @p def when @p j is no object, the member is missing or has another type. */
template <class T>
T jget(const nlohmann::json& j, const char* key, const T& def)
{
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    if (it == j.end()) return def;
    try
    {
        return it->template get<T>();
    }
    catch (const nlohmann::json::exception&)
    {
        return def;
    }
}

/** @brief Overload for string literals as the default. */
inline std::string jget(const nlohmann::json& j, const char* key, const char* def) { return jget<std::string>(j, key, std::string(def)); }

} // namespace serve
} // namespace ftc
