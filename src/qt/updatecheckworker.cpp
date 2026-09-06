// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatecheckworker.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <string>
#include <vector>

QVariantMap UpdateInfoToVariantMap(const UniValue& result)
{
    QVariantMap info;
    if (!result.isObject()) return info;

    // Every key, rather than the ones this happens to know about. The reader
    // asks for what it needs; anything it does not need costs a map entry.
    const std::vector<std::string>& keys{result.getKeys()};
    const std::vector<UniValue>& values{result.getValues()};
    for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        const QString key{QString::fromStdString(keys[i])};
        switch (values[i].getType()) {
        case UniValue::VBOOL:
            info[key] = values[i].get_bool();
            break;
        case UniValue::VNUM:
            info[key] = static_cast<qlonglong>(values[i].get_int64());
            break;
        case UniValue::VSTR:
            info[key] = QString::fromStdString(values[i].get_str());
            break;
        default:
            // Nothing else appears in an update check result today. Skipping is
            // right rather than stringifying: a reader asking for a key that is
            // not there gets an empty value, which is what it would have got
            // from a field this could not represent anyway.
            break;
        }
    }
    return info;
}

void UpdateCheckWorker::check()
{
    Q_EMIT checked(UpdateInfoToVariantMap(m_node.checkForUpdates()));
}
