// Copyright 2018 The Beam Team
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

#include "fly_client.h"
#include "../utility/executor.h"

namespace beam {
namespace proto {

FlyClient::NetworkStd::~NetworkStd()
{
    Disconnect();
}

void FlyClient::NetworkStd::Connect()
{
    if (m_Connections.size() == m_Cfg.m_vNodes.size())
    {
        // force (re) connect
        for (ConnectionList::iterator it = m_Connections.begin(); m_Connections.end() != it; ++it)
        {
            Connection& c = *it;
            if (c.IsLive() && c.IsSecureOut())
                continue;

            c.ResetAll();
            if (m_Cfg.m_UseProxy)
                c.Connect(c.m_Addr, m_Cfg.m_ProxyAddr);
            else
                c.Connect(c.m_Addr);
        }
    }
    else
    {
        Disconnect();

        for (size_t i = 0; i < m_Cfg.m_vNodes.size(); i++)
        {
            Connection* pConn = new Connection(*this);
            pConn->m_Addr = m_Cfg.m_vNodes[i];
            if (m_Cfg.m_UseProxy) pConn->Connect(pConn->m_Addr, m_Cfg.m_ProxyAddr);
            else pConn->Connect(pConn->m_Addr);
        }
    }
}

void FlyClient::NetworkStd::Disconnect()
{
    while (!m_Connections.empty())
        delete &m_Connections.front();
}

FlyClient::NetworkStd::Connection::Connection(NetworkStd& x)
    : m_This(x)
{
    m_This.m_Connections.push_back(*this);
    ResetVars();
}

FlyClient::NetworkStd::Connection::~Connection()
{
    ResetInternal();
    m_This.m_Connections.erase(ConnectionList::s_iterator_to(*this));

}

bool FlyClient::NetworkStd::Connection::ShouldSync() const
{
    Block::SystemState::Full sTip;
    return !m_This.m_Client.get_History().get_Tip(sTip) || (sTip.m_ChainWork < m_Tip.m_ChainWork);
}

void FlyClient::NetworkStd::Connection::ResetVars()
{
    ZeroObject(m_Tip);
    m_LoginFlags = 0;
    m_Flags = 0;
    m_NodeID = Zero;
}

void FlyClient::NetworkStd::Connection::ResetInternal()
{
    m_pSync.reset();
	KillTimer();

    if (Flags::Owned & m_Flags)
        m_This.m_Client.OnOwnedNode(m_NodeID, false);

    if (Flags::ReportedConnected & m_Flags)
        m_This.OnNodeConnected(false);

    while (!m_lst.empty())
    {
        RequestNode& n = m_lst.front();
        m_lst.pop_front();
        m_This.m_lst.push_back(n);
    }
}

void FlyClient::NetworkStd::Connection::OnConnectedSecure()
{
	SendLogin();

    if (!(Flags::ReportedConnected & m_Flags))
    {
        m_Flags |= Flags::ReportedConnected;
        m_This.OnNodeConnected(true);
    }
}

void FlyClient::NetworkStd::Connection::SetupLogin(Login& msg)
{
    msg.m_Flags |= LoginFlags::MiningFinalization | LoginFlags::SendPeers;
    m_This.OnLoginSetup(msg);
}

void FlyClient::NetworkStd::Connection::OnDisconnect(const DisconnectReason& dr)
{
    m_This.OnConnectionFailed(dr);
	ResetAll();
    SetTimer(m_This.m_Cfg.m_ReconnectTimeout_ms);
}

void FlyClient::NetworkStd::Connection::ResetAll()
{
	NodeConnection::Reset();
	ResetInternal();
	ResetVars();
}

void FlyClient::NetworkStd::Connection::SetTimer(uint32_t timeout_ms)
{
    if (!m_pTimer)
        m_pTimer = io::Timer::create(io::Reactor::get_Current());

    m_pTimer->start(timeout_ms, false, [this]() { OnTimer(); });
}

void FlyClient::NetworkStd::Connection::KillTimer()
{
    if (m_pTimer)
        m_pTimer->cancel();
}

void FlyClient::NetworkStd::Connection::OnTimer()
{
    if (IsLive())
    {
        if (m_This.m_Cfg.m_PollPeriod_ms)
        {
            ResetAll();
            uint32_t timeout_ms = std::max(Rules::get().DA.Target_s * 1000, m_This.m_Cfg.m_PollPeriod_ms);
            SetTimer(timeout_ms);
        }
    }
    else
    {
        ResetAll();
        if (m_This.m_Cfg.m_UseProxy) Connect(m_Addr, m_This.m_Cfg.m_ProxyAddr);
        else Connect(m_Addr);
    }
}

void FlyClient::NetworkStd::Connection::OnMsg(Authentication&& msg)
{
    NodeConnection::OnMsg(std::move(msg));

    switch (msg.m_IDType)
    {
    case IDType::Node:
        {
            if (Flags::Node & m_Flags)
                ThrowUnexpected();

            m_Flags |= Flags::Node;
            m_NodeID = msg.m_ID;

            Key::IKdf::Ptr pKdf;
            m_This.m_Client.get_Kdf(pKdf);
            if (pKdf)
            {
                ProveKdfObscured(*pKdf, IDType::Owner);
            }
            else
            {
                Key::IPKdf::Ptr ownerKdf;
                m_This.m_Client.get_OwnerKdf(ownerKdf);
                if (ownerKdf)
                {
                    ProvePKdfObscured(*ownerKdf, IDType::Viewer);
                }
            }
        }
        break;

    case IDType::Viewer:
        {
            if ((Flags::Owned & m_Flags) || !(Flags::Node & m_Flags))
                ThrowUnexpected();

            Key::IPKdf::Ptr pubKdf;
            m_This.m_Client.get_OwnerKdf(pubKdf);
            if (!(pubKdf && IsPKdfObscured(*pubKdf, msg.m_ID)))
                ThrowUnexpected();

            //  viewer confirmed!
            m_Flags |= Flags::Owned;
            m_This.m_Client.OnOwnedNode(m_NodeID, true);
        }
        break;

    default: // suppress warning
        break;
    }
}

void FlyClient::NetworkStd::Connection::OnMsg(GetBlockFinalization&& msg)
{
    if (!(Flags::Owned & m_Flags))
        ThrowUnexpected();

    Key::IKdf::Ptr pKdf;
    m_This.m_Client.get_Kdf(pKdf);
    if (!pKdf)
        ThrowUnexpected(); // ?!

    Block::Builder bb(0, *pKdf, *pKdf, msg.m_Height);
    bb.AddCoinbaseAndKrn();
    bb.AddFees(msg.m_Fees);

    proto::BlockFinalization msgOut;
    msgOut.m_Value.reset(new Transaction);
    bb.m_Txv.MoveInto(*msgOut.m_Value);
    msgOut.m_Value->m_Offset = -bb.m_Offset;
    msgOut.m_Value->Normalize();

    Send(msgOut);
}

void FlyClient::NetworkStd::Connection::OnLogin(Login&& msg)
{
    m_LoginFlags = msg.m_Flags;
    AssignRequests();

    if (LoginFlags::Bbs & m_LoginFlags)
        for (BbsSubscriptions::const_iterator it = m_This.m_BbsSubscriptions.begin(); m_This.m_BbsSubscriptions.end() != it; ++it)
        {
            proto::BbsSubscribe msgOut;
            msgOut.m_TimeFrom = it->second.second;
            msgOut.m_Channel = it->first;
            msgOut.m_On = true;
            Send(msgOut);
        }
}

void FlyClient::NetworkStd::Connection::OnMsg(NewTip&& msg)
{
	if (msg.m_Description.m_Height < Rules::HeightGenesis)
		return; // ignore

    if (m_Tip == msg.m_Description)
        return; // redundant msg

    if (msg.m_Description.m_ChainWork <= m_Tip.m_ChainWork)
        ThrowUnexpected();

    if (!(msg.m_Description.IsValid()))
        ThrowUnexpected();

    if (m_pSync && m_pSync->m_vConfirming.empty() && !m_pSync->m_TipBeforeGap.m_Height && !m_Tip.IsNext(msg.m_Description))
        m_pSync->m_TipBeforeGap = m_Tip;

    bool shouldReassignRequests = !m_Tip.IsValid();

    m_Tip = msg.m_Description;

    if (!m_pSync)
    {
        if (ShouldSync())
        {
            StartSync();
        }
        else
        {
            m_This.m_Client.OnTipUnchanged();
            if (shouldReassignRequests)
            {
                AssignRequests();
            }
        }
    }
}

void FlyClient::NetworkStd::Connection::StartSync()
{
    assert(ShouldSync());
    KillTimer();

    Block::SystemState::Full sTip;
    m_This.m_Client.get_History().get_Tip(sTip);
    if (sTip.IsNext(m_Tip))
    {
        // simple case
        m_This.m_Client.get_History().AddStates(&m_Tip, 1);
        PrioritizeSelf();
        AssignRequests();
        m_This.m_Client.OnNewTip();
    }
    else
    {
        // starting search
        // starting search
        m_pSync.reset(new SyncCtx);
        m_pSync->m_LowHeight = m_Tip.m_Height;
        SearchBelow(m_Tip.m_Height, 1);
    }
}

void FlyClient::NetworkStd::Connection::SearchBelow(Height h, uint32_t nCount)
{
    assert(ShouldSync() && m_pSync && m_pSync->m_vConfirming.empty());
    assert(nCount);

    struct Walker :public Block::SystemState::IHistory::IWalker
    {
        std::vector<Block::SystemState::Full> m_vStates;
        uint32_t m_Count;

        virtual bool OnState(const Block::SystemState::Full& s) override
        {
            m_vStates.push_back(s);
            return m_vStates.size() < m_Count;
        }
    } w;

    w.m_Count = nCount;
    w.m_vStates.reserve(nCount);
    m_This.m_Client.get_History().Enum(w, &h);

    if (w.m_vStates.empty())
    {
        ZeroObject(m_pSync->m_Confirmed);
        RequestChainworkProof();
    }
    else
    {
        GetCommonState msg;
        msg.m_IDs.resize(w.m_vStates.size());

        for (size_t i = 0; i < msg.m_IDs.size(); i++)
            w.m_vStates[i].get_ID(msg.m_IDs[i]);

        Send(msg);

        m_pSync->m_vConfirming.swap(w.m_vStates);
    }
}

void FlyClient::NetworkStd::Connection::OnMsg(ProofCommonState&& msg)
{
    if (!m_pSync)
        ThrowUnexpected();

    std::vector<Block::SystemState::Full> vStates = std::move(m_pSync->m_vConfirming);
    if (vStates.empty())
        ThrowUnexpected();

    if (!ShouldSync())
    {
        m_pSync.reset();
        return; // other connection was faster
    }

    size_t iState;
    for (iState = 0; ; iState++)
    {
        if (vStates.size() == iState)
        {
            // not found. Theoretically it's possible that the current tip is lower than the requested range (but highly unlikely)
            if (m_Tip.m_Height > vStates.back().m_Height)
                ThrowUnexpected();

            SearchBelow(m_Tip.m_Height, 1); // restart
            return;

        }
        if (vStates[iState].m_Height == msg.m_ID.m_Height)
            break;
    }

    if (!m_Tip.IsValidProofState(msg.m_ID, msg.m_Proof))
        ThrowUnexpected();

    if ((m_pSync->m_LowHeight < vStates.front().m_Height) && iState)
        SearchBelow(m_pSync->m_LowHeight + 1, 1); // restart the search from this height
    else
    {
        const Block::SystemState::Full& s = vStates[iState];
        Merkle::Hash hv;
        s.get_Hash(hv);
        if (hv != msg.m_ID.m_Hash)
        {
            if (iState != vStates.size() - 1)
                ThrowUnexpected(); // the disproof should have been for the last requested state

            SearchBelow(vStates.back().m_Height, static_cast<uint32_t>(vStates.size() * 2)); // all the range disproven. Search below
        }
        else
        {
            m_pSync->m_Confirmed = s;
            RequestChainworkProof();
        }
    }
}

struct FlyClient::NetworkStd::Connection::StateArray
{
    std::vector<Block::SystemState::Full> m_vec;

    bool Find(const Block::SystemState::Full&) const;
};

bool FlyClient::NetworkStd::Connection::StateArray::Find(const Block::SystemState::Full& s) const
{
    struct Cmp {
        bool operator () (const Block::SystemState::Full& s, Height h) const { return s.m_Height < h; }
    };

    // the array should be sorted (this is verified by chaiworkproof verification)
    std::vector<Block::SystemState::Full>::const_iterator it = std::lower_bound(m_vec.begin(), m_vec.end(), s.m_Height, Cmp());
    return (m_vec.end() != it) && (*it == s);
}

void FlyClient::NetworkStd::Connection::RequestChainworkProof()
{
    assert(ShouldSync() && m_pSync && m_pSync->m_vConfirming.empty());

    if (Flags::Owned & m_Flags)
    {
        // for trusted nodes this is not required. Go straight to finish
        SyncCtx::Ptr pSync = std::move(m_pSync);
        StateArray arr;
        PostChainworkProof(arr, pSync->m_Confirmed.m_Height);
    }
    else
    {
        GetProofChainWork msg;
        msg.m_LowerBound = m_pSync->m_Confirmed.m_ChainWork;
        Send(msg);

        m_pSync->m_TipBeforeGap.m_Height = 0;
        m_pSync->m_LowHeight = m_pSync->m_Confirmed.m_Height;
    }
}

void FlyClient::NetworkStd::Connection::OnMsg(ProofChainWork&& msg)
{
    if (!m_pSync || !m_pSync->m_vConfirming.empty())
        ThrowUnexpected();

    if (msg.m_Proof.m_LowerBound != m_pSync->m_Confirmed.m_ChainWork)
        ThrowUnexpected();

    Block::SystemState::Full sTip;
    if (!msg.m_Proof.IsValid(&sTip))
        ThrowUnexpected();

    if (sTip != m_Tip)
        ThrowUnexpected();

    SyncCtx::Ptr pSync = std::move(m_pSync);

    if (!ShouldSync())
        return;

    // Unpack the proof, convert it to one sorted array. For convenience
    StateArray arr;
	msg.m_Proof.UnpackStates(arr.m_vec);

    if (pSync->m_TipBeforeGap.m_Height && pSync->m_Confirmed.m_Height)
    {
        // Since there was a gap in the tips reported by the node (which is typical in case of reorgs) - there is a possibility that our m_Confirmed is no longer valid.
        // If either the m_Confirmed ot the m_TipBeforeGap are mentioned in the chainworkproof - then there's no problem with reorg.
        // And since chainworkproof usually contains a "tail" of consecutive headers - there should be no problem, unless the reorg is huge
        // Otherwise sync should be repeated
        if (!arr.Find(pSync->m_TipBeforeGap) &&
            !arr.Find(pSync->m_Confirmed))
        {
            StartSync(); // again
            return;
        }
    }

    PostChainworkProof(arr, pSync->m_LowHeight);
}

void FlyClient::NetworkStd::Connection::PostChainworkProof(const StateArray& arr, Height hLowHeight)
{
    struct Walker :public Block::SystemState::IHistory::IWalker
    {
        Height m_LowHeight;
        Height m_LowErase;
        const StateArray* m_pArr;

        virtual bool OnState(const Block::SystemState::Full& s) override
        {
            if (s.m_Height <= m_LowHeight)
                return false;

            if (m_pArr->Find(s))
                return false;

            m_LowErase = s.m_Height;
            return true;
        }
    } w;

    w.m_LowErase = MaxHeight;
    w.m_LowHeight = hLowHeight;
    w.m_pArr = &arr;

    m_This.m_Client.get_History().Enum(w, NULL);

    if (w.m_LowErase != MaxHeight)
    {
        m_This.m_Client.get_History().DeleteFrom(w.m_LowErase);

        // if more connections are opened simultaneously - notify them
        for (ConnectionList::iterator it = m_This.m_Connections.begin(); m_This.m_Connections.end() != it; ++it)
        {
            const Connection& c = *it;
            if (c.m_pSync)
                std::setmin(c.m_pSync->m_LowHeight, w.m_LowErase - 1);
        }

        m_This.m_Client.OnRolledBack();
    }

    if (arr.m_vec.empty())
        m_This.m_Client.get_History().AddStates(&m_Tip, 1);
    else
        m_This.m_Client.get_History().AddStates(&arr.m_vec.front(), arr.m_vec.size());
    PrioritizeSelf();
    m_This.m_Client.OnNewTip(); // finished!
    AssignRequests();
}


void FlyClient::NetworkStd::Connection::PrioritizeSelf()
{
    m_This.m_Connections.erase(ConnectionList::s_iterator_to(*this));
    m_This.m_Connections.push_front(*this);
}

void FlyClient::INetwork::PostRequest(Request& r, Request::IHandler& h)
{
    assert(!r.m_pTrg);
    r.m_pTrg = &h;
    PostRequestInternal(r);
}

void FlyClient::NetworkStd::PostRequestInternal(Request& r)
{
    assert(r.m_pTrg);

    RequestNode* pNode = m_lst.Create_back();
    pNode->m_pRequest = &r;

    OnNewRequests();
}

void FlyClient::NetworkStd::OnNewRequests()
{
    for (ConnectionList::iterator it = m_Connections.begin(); m_Connections.end() != it; ++it)
    {
        Connection& c = *it;
        if (c.IsLive() && c.IsSecureOut())
        {
            c.AssignRequests();
            break;
        }
    }
}

bool FlyClient::NetworkStd::Connection::IsAtTip() const
{
    Block::SystemState::Full sTip;
    return m_This.m_Client.get_History().get_Tip(sTip) && (sTip == m_Tip);
}

void FlyClient::NetworkStd::Connection::AssignRequests()
{
    for (RequestList::iterator it = m_This.m_lst.begin(); m_This.m_lst.end() != it; )
        AssignRequest(*it++);

    if (m_lst.empty() && m_This.m_Cfg.m_PollPeriod_ms)
        SetTimer(m_This.m_Cfg.m_CloseConnectionDelay_ms); // this should allow to get sbbs messages
    else
        KillTimer();
}

void FlyClient::NetworkStd::Connection::AssignRequest(RequestNode& n)
{
    assert(n.m_pRequest);
    if (!n.m_pRequest->m_pTrg)
    {
        m_This.m_lst.Delete(n);
        return;
    }

    switch (n.m_pRequest->get_Type())
    {
#define THE_MACRO(type, msgOut, msgIn) \
    case Request::Type::type: \
        { \
            Request##type& req = Cast::Up<Request##type>(*n.m_pRequest); \
            if (!IsSupported(req)) \
                return; \
            SendRequest(req); \
        } \
        break;

    REQUEST_TYPES_All(THE_MACRO)
#undef THE_MACRO

    default: // ?!
        m_This.m_lst.Finish(n);
        return;
    }

    m_This.m_lst.erase(RequestList::s_iterator_to(n));
    m_lst.push_back(n);
}

void FlyClient::NetworkStd::RequestList::Finish(RequestNode& n)
{
    assert(n.m_pRequest);
    if (n.m_pRequest->m_pTrg)
        n.m_pRequest->m_pTrg->OnComplete(*n.m_pRequest);
    Delete(n);
}

FlyClient::Request& FlyClient::NetworkStd::Connection::get_FirstRequestStrict(Request::Type x)
{
    auto& r = get_FirstRequest();

    if (r.get_Type() != x)
        ThrowUnexpected();

    return r;
}

FlyClient::Request& FlyClient::NetworkStd::Connection::get_FirstRequest()
{
    if (m_lst.empty())
        ThrowUnexpected();
    RequestNode& n = m_lst.front();
    assert(n.m_pRequest);

    return *n.m_pRequest;
}

#define THE_MACRO_SWAP_FIELD(type, name) std::swap(req.m_Res.m_##name, msg.m_##name);
#define THE_MACRO(type, msgOut, msgIn) \
void FlyClient::NetworkStd::Connection::OnMsg(msgIn&& msg) \
{  \
    Request##type& req = Cast::Up<Request##type>(get_FirstRequestStrict(Request::Type::type)); \
    BeamNodeMsg_##msgIn(THE_MACRO_SWAP_FIELD) \
    OnRequestData(req); \
    OnFirstRequestDone(IsSupported(req)); \
}

REQUEST_TYPES_All(THE_MACRO)
#undef THE_MACRO
#undef THE_MACRO_SWAP_FIELD

bool FlyClient::NetworkStd::Connection::IsSupported(RequestUtxo& req)
{
    return IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestUtxo& req)
{
    for (size_t i = 0; i < req.m_Res.m_Proofs.size(); i++)
        if (!m_Tip.IsValidProofUtxo(req.m_Msg.m_Utxo, req.m_Res.m_Proofs[i]))
            ThrowUnexpected();
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestKernel& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestKernel& req)
{
    if (!req.m_Res.m_Proof.empty())
        if (!m_Tip.IsValidProofKernel(req.m_Msg.m_ID, req.m_Res.m_Proof))
            ThrowUnexpected();
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestKernel2& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestAsset& req)
{
    if (req.m_Res.m_Info.m_Owner != Zero) // valid asset info
    {
        if (req.m_Msg.m_Owner != Zero && req.m_Msg.m_Owner != req.m_Res.m_Info.m_Owner)
            ThrowUnexpected();

        if (req.m_Msg.m_AssetID != Asset::s_InvalidID && req.m_Msg.m_AssetID != req.m_Res.m_Info.m_ID)
            ThrowUnexpected();
    }

    if (!req.m_Res.m_Proof.empty())
        if (!m_Tip.IsValidProofAsset(req.m_Res.m_Info, req.m_Res.m_Proof))
            ThrowUnexpected();
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestAsset& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestKernel2& req)
{
    if (req.m_Res.m_Kernel)
    {
        ECC::Point::Native exc;

        if (!req.m_Res.m_Kernel->IsValid(req.m_Res.m_Height, exc))
        {
            ThrowUnexpected();
        }
    }
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestEvents& req)
{
    return (Flags::Owned & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestEvents& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestTransaction& req)
{
    return (LoginFlags::SpreadingTransactions & m_LoginFlags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestTransaction& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestShieldedList& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestShieldedList& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestProofShieldedInp& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestProofShieldedInp& req)
{
    if (!req.m_Res.m_Proof.empty())
    {
        ShieldedTxo::DescriptionInp desc;
        desc.m_Height = req.m_Res.m_Height;
        desc.m_SpendPk = req.m_Msg.m_SpendPk;

        if (!m_Tip.IsValidProofShieldedInp(desc, req.m_Res.m_Proof))
        {
            ThrowUnexpected();
        }
    }
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestProofShieldedOutp& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestProofShieldedOutp& req)
{
    if (!req.m_Res.m_Proof.empty())
    {
        ShieldedTxo::DescriptionOutp desc;
        desc.m_ID = req.m_Res.m_ID;
        desc.m_Height = req.m_Res.m_Height;
        desc.m_SerialPub = req.m_Msg.m_SerialPub;
        desc.m_Commitment = req.m_Res.m_Commitment;

        if (!m_Tip.IsValidProofShieldedOutp(desc, req.m_Res.m_Proof))
        {
            ThrowUnexpected();
        }
    }
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestStateSummary& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestStateSummary& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestEnumHdrs& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

bool details::ExtraData<proto::HdrPack>::DecodeAndCheck(const HdrPack& msg)
{
    if (msg.m_vElements.empty())
        return true; // this is allowed

    // PoW verification is heavy for big packs. Do it in parallel
    std::vector<Block::SystemState::Full> v;
    v.resize(msg.m_vElements.size());

    Cast::Down<Block::SystemState::Sequence::Prefix>(v.front()) = msg.m_Prefix;
    Cast::Down<Block::SystemState::Sequence::Element>(v.front()) = msg.m_vElements.back();

    for (size_t i = 1; i < msg.m_vElements.size(); i++)
    {
        Block::SystemState::Full& s0 = v[i - 1];
        Block::SystemState::Full& s1 = v[i];

        s0.get_Hash(s1.m_Prev);
        s1.m_Height = s0.m_Height + 1;
        Cast::Down<Block::SystemState::Sequence::Element>(s1) = msg.m_vElements[msg.m_vElements.size() - i - 1];
        s1.m_ChainWork = s0.m_ChainWork + s1.m_PoW.m_Difficulty;
    }

    struct MyTask
        :public Executor::TaskSync
    {
        const Block::SystemState::Full* m_pV;
        uint32_t m_Count;
        bool m_Valid;

        virtual ~MyTask() {}

        virtual void Exec(Executor::Context& ctx) override
        {
            uint32_t i0, nCount;
            ctx.get_Portion(i0, nCount, m_Count);
            TestRange(i0, nCount);
        }

        void TestRange(uint32_t i0, uint32_t nCount)
        {
            nCount += i0;
            for (; i0 < nCount; i0++)
                if (!m_pV[i0].IsValid())
                    m_Valid = false;
        }
    };

    MyTask t;
    t.m_pV = &v.front();
    t.m_Count = static_cast<uint32_t>(v.size());
    t.m_Valid = true;

    if (Executor::s_pInstance)
        Executor::s_pInstance->ExecAll(t);
    else
        t.TestRange(0, t.m_Count);

    if (t.m_Valid)
        m_vStates = std::move(v);

    return t.m_Valid;
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestEnumHdrs& req)
{
    if (!req.DecodeAndCheck(req.m_Res))
        ThrowUnexpected();
}

void FlyClient::NetworkStd::Connection::OnMsg(DataMissing&& msg)
{
    auto& r = get_FirstRequest();
    auto type = r.get_Type();
    if (type == Request::Type::EnumHdrs)
    {
        auto& req = Cast::Up<RequestEnumHdrs>(get_FirstRequestStrict(Request::Type::EnumHdrs));
        OnFirstRequestDone(IsSupported(req));
    }
    else if (type == Request::Type::BodyPack)
    {
        auto& req = Cast::Up<RequestBodyPack>(get_FirstRequestStrict(Request::Type::BodyPack));
        OnFirstRequestDone(IsSupported(req));
    }
    else
    {
        ThrowUnexpected();
    }
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestContractVars& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestContractVars& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestContractLogs& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestContractLogs& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestContractVar& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestContractVar& req)
{
    if (!req.m_Res.m_Proof.empty() && !m_Tip.IsValidProofContract(req.m_Msg.m_Key, req.m_Res.m_Value, req.m_Res.m_Proof))
        ThrowUnexpected();
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestContractLogProof& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestContractLogProof& req)
{
    // can't validate the proof in-place, the appropriate header is not part of the reply
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestShieldedOutputsAt& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestShieldedOutputsAt& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestBodyPack& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestBodyPack& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestBody& req)
{
    return (Flags::Node & m_Flags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestBody& req)
{
}

bool FlyClient::NetworkStd::Connection::IsSupported(RequestBbsMsg& req)
{
    return (LoginFlags::Bbs & m_LoginFlags) && IsAtTip();
}

void FlyClient::NetworkStd::Connection::SendRequest(RequestBbsMsg& req)
{
	Send(req.m_Msg);

	Ping msg2(Zero);
    Send(msg2);
}

void FlyClient::NetworkStd::Connection::OnRequestData(RequestBbsMsg& req)
{
}

void FlyClient::NetworkStd::Connection::OnFirstRequestDone(bool bStillSupported)
{
    RequestNode& n = m_lst.front();
    assert(n.m_pRequest);

    if (n.m_pRequest->m_pTrg)
    {
        if (!bStillSupported)
        {
            // should retry
            m_lst.erase(RequestList::s_iterator_to(n));
            m_This.m_lst.push_back(n);
            m_This.OnNewRequests();
            return;
        }

        m_lst.Finish(n);
    }
    else
        m_lst.Delete(n); // aborted already

    if (m_lst.empty() && m_This.m_Cfg.m_PollPeriod_ms)
    {
        SetTimer(0);
    }
}

void FlyClient::NetworkStd::BbsSubscribe(BbsChannel ch, Timestamp ts, IBbsReceiver* p)
{
    BbsSubscriptions::iterator it = m_BbsSubscriptions.find(ch);
    if (m_BbsSubscriptions.end() == it)
    {
        if (!p)
            return;

        m_BbsSubscriptions.insert(std::make_pair(ch, std::make_pair(p, ts)));
    }
    else
    {
        if (p)
        {
            it->second.first = p;
            it->second.second = ts;
            return;
        }

        m_BbsSubscriptions.erase(it);
    }

    proto::BbsSubscribe msg;
    msg.m_TimeFrom = ts;
    msg.m_Channel = ch;
    msg.m_On = (nullptr != p);

    for (ConnectionList::iterator it2 = m_Connections.begin(); m_Connections.end() != it2; ++it2)
        if (it2->IsLive() && it2->IsSecureOut())
            it2->Send(msg);
}

void FlyClient::NetworkStd::Connection::OnMsg(BbsMsg&& msg)
{
    BbsSubscriptions::iterator it = m_This.m_BbsSubscriptions.find(msg.m_Channel);
    if (m_This.m_BbsSubscriptions.end() != it)
    {
        it->second.second = msg.m_TimePosted;

        assert(it->second.first);
        it->second.first->OnMsg(std::move(msg));
    }
}

void FlyClient::NetworkStd::Connection::OnMsg(EventsSerif&& msg)
{
    if (!(Flags::Owned & m_Flags))
        ThrowUnexpected();

    // TODO: handle complex situation, where multiple owned nodes are connected
    m_This.m_Client.OnEventsSerif(msg.m_Value, msg.m_Height);
}

void FlyClient::NetworkStd::Connection::OnMsg(PeerInfo&& msg)
{
    m_This.m_Client.OnNewPeer(msg.m_ID, msg.m_LastAddr);
}

} // namespace proto
} // namespace beam
