// Copyright 2020 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "wallet_api.h"

namespace beam::wallet
{
    WalletApi::WalletApi(
            IWalletApiHandler& handler,
            ACL acl,
            std::string appid,
            IWalletDB::Ptr wdb,
            Wallet::Ptr wallet,
            ISwapsProvider::Ptr swaps,
            IShadersManager::Ptr contracts
        )
        : ApiBase(handler, std::move(acl), std::move(appid))
        , _wdb(std::move(wdb))
        , _wallet(std::move(wallet))
        , _swaps(std::move(swaps))
        , _contracts(std::move(contracts))
    {
        // MUST BE SAFE TO CALL FROM ANY THREAD
        // Don't do anything with walletdb, providers &c.
        #define REG_FUNC(api, name, writeAccess, isAsync, appsAllowed)    \
        _methods[name] = {                                                \
            [this] (const JsonRpcId &id, const json &msg) {               \
                auto parseRes = onParse##api(id, msg);                    \
                onHandle##api(id, parseRes.first);                        \
            },                                                            \
            [this] (const JsonRpcId &id, const json &msg) -> MethodInfo { \
                auto parseRes = onParse##api(id, msg);                    \
                return parseRes.second;                                   \
            },                                                            \
            writeAccess, isAsync, appsAllowed                             \
        };
        WALLET_API_METHODS(REG_FUNC)
        #undef REG_FUNC
    }
}
