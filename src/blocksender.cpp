// Copyright (c) 2016 The Bitcoin XT developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "blocksender.h"
#include "protocol.h"
#include "chain.h"
#include "chainparams.h"
#include "util.h"
#include "net.h"
#include "xthin.h"
#include "merkleblock.h"
#include "main.h" // ReadBlockFromDisk
#include <vector>

BlockSender::BlockSender() {
}

bool BlockSender::isBlockType(int t) const {
    return t == MSG_BLOCK || t == MSG_FILTERED_BLOCK
        || t == MSG_THINBLOCK || t == MSG_XTHINBLOCK;
}

bool BlockSender::canSend(const CChain& activeChain, const CBlockIndex& block,
        CBlockIndex *pindexBestHeader)
{
    // Pruned nodes may have deleted the block, so check whether
    // it's available before trying to send.
    if (!(block.nStatus & BLOCK_HAVE_DATA))
        return false;

    if (activeChain.Contains(&block))
        return true;

    static const int nOneMonth = 30 * 24 * 60 * 60;
    // To prevent fingerprinting attacks, only send blocks outside of the active
    // chain if they are valid, and no more than a month older (both in time, and in
    // best equivalent proof of work) than the best header chain we know about.
    bool send = block.IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
        (pindexBestHeader->GetBlockTime() - block.GetBlockTime() < nOneMonth) &&
        (GetBlockProofEquivalentTime(*pindexBestHeader, block, *pindexBestHeader, Params().GetConsensus()) < nOneMonth);
    if (!send)
        LogPrintf("ignoring request for old block that isn't in the main chain\n");

    return send;
}

void BlockSender::send(const CChain& activeChain, CNode& node,
        const CBlockIndex& blockIndex, const CInv& inv)
{
    sendBlock(node, blockIndex, inv.type);
    triggerNextRequest(activeChain, inv, node);
}

// Trigger the peer node to send a getblocks request for the next batch of inventory
void BlockSender::triggerNextRequest(const CChain& activeChain, const CInv& inv, CNode& node) {

    if (inv.hash != node.hashContinue)
        return;

    // Bypass PushInventory, this must send even if redundant,
    // and we want it right after the last block so they don't
    // wait for other stuff first.
    std::vector<CInv> vInv;
    vInv.push_back(CInv(MSG_BLOCK, activeChain.Tip()->GetBlockHash()));
    node.PushMessage("inv", vInv);
    node.hashContinue.SetNull();
}

bool thinIsSmaller(const CBlock& b, const XThinBlock& x) {
    return GetSerializeSize(x, SER_NETWORK, PROTOCOL_VERSION)
        < GetSerializeSize(b, SER_NETWORK, PROTOCOL_VERSION);
}

void BlockSender::sendBlock(CNode& node,
        const CBlockIndex& blockIndex, int invType)
{

    // Send block from disk
    CBlock block;
    if (!readBlockFromDisk(block, &blockIndex))
        assert(!"cannot load block from disk");

    assert(!block.IsNull());

    // We only support MSG_XTHINBLOCK, if peer wants MSG_THINBLOCK,
    // fallback to full one.
    if (invType == MSG_BLOCK || invType == MSG_THINBLOCK)
    {
        node.PushMessage("block", block);
        return;
    }

    CBloomFilter filter;
    {
        LOCK(node.cs_filter);
        assert(bool(node.xthinFilter.get())); // a filter is always allocated.
        filter = *node.xthinFilter;
    }


    if (invType == MSG_XTHINBLOCK) {
        try {
            XThinBlock thinb(block, filter);
            if (thinIsSmaller(block, thinb))
                node.PushMessage("xthinblock", thinb);
            else
                node.PushMessage("block", block);
        }
        catch (const xthin_collision_error& e) {
            LogPrintf("tx collision in thin block %s\n",
                    block.GetHash().ToString());

            // fall back to full block
            node.PushMessage("block", block);
        }
        return;
    }

    // MSG_FILTERED_BLOCK

    CMerkleBlock merkleBlock(block, filter);
    node.PushMessage("merkleblock", merkleBlock);
    // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
    // This avoids hurting performance by pointlessly requiring a round-trip
    // Note that there is currently no way for a node to request any single transactions we didn't send here -
    // they must either disconnect and retry or request the full block.
    // Thus, the protocol spec specified allows for us to provide duplicate txn here,
    // however we MUST always provide at least what the remote peer needs
    LOCK(node.cs_inventory);
    typedef std::pair<unsigned int, uint256> PairType;
    BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
        if (!node.filterInventoryKnown.contains(pair.second))
            node.PushMessage("tx", block.vtx[pair.first]);
}

void BlockSender::sendReReqReponse(CNode& node, const CBlockIndex& blockIndex,
        const XThinReRequest& req)
{
    CBlock block;
    if (!readBlockFromDisk(block, &blockIndex))
        assert(!"cannot load block from disk");

    XThinReReqResponse resp(block, req.txRequesting);
    node.PushMessage("xblocktx", resp);
}

bool BlockSender::readBlockFromDisk(CBlock& block, const CBlockIndex* pindex) {
    return ::ReadBlockFromDisk(block, pindex);
}
