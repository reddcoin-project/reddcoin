// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatecheckworker.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <string>

namespace {
QString GetString(const UniValue& value, const std::string& key)
{
    return value.exists(key) ? QString::fromStdString(value[key].get_str()) : QString{};
}
} // namespace

void UpdateCheckWorker::check()
{
    const UniValue result{m_node.checkForUpdates()};

    QVariantMap info;
    info["updateavailable"] = result.exists("updateavailable") && result["updateavailable"].get_bool();
    info["localversion"] = GetString(result, "localversion");
    info["remoteversion"] = GetString(result, "remoteversion");
    info["message"] = GetString(result, "message");
    info["warning"] = GetString(result, "warning");
    info["officialDownloadLink"] = GetString(result, "officialDownloadLink");
    info["errors"] = GetString(result, "errors");

    Q_EMIT checked(info);
}
