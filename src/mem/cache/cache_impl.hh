/*
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Erik Hallnor
 *          Dave Greene
 *          Nathan Binkert
 */

/**
 * @file
 * Cache definitions.
 */

#include <assert.h>
#include <math.h>

#include <cassert>
#include <iostream>
#include <string>

#include "sim/host.hh"
#include "base/misc.hh"
#include "cpu/smt.hh"

#include "mem/cache/cache.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/miss/mshr.hh"
#include "mem/cache/prefetch/prefetcher.hh"

#include "sim/sim_exit.hh" // for SimExitEvent

template<class TagStore, class Buffering, class Coherence>
bool
Cache<TagStore,Buffering,Coherence>::
doTimingAccess(PacketPtr pkt, CachePort *cachePort, bool isCpuSide)
{
    if (isCpuSide)
    {
        if (pkt->isWrite() && (pkt->req->isLocked())) {
            pkt->req->setScResult(1);
        }
        access(pkt);

    }
    else
    {
        if (pkt->isResponse())
            handleResponse(pkt);
        else {
            //Check if we should do the snoop
            if (pkt->flags & SNOOP_COMMIT)
                snoop(pkt);
        }
    }
    return true;
}

template<class TagStore, class Buffering, class Coherence>
Tick
Cache<TagStore,Buffering,Coherence>::
doAtomicAccess(PacketPtr pkt, bool isCpuSide)
{
    if (isCpuSide)
    {
        probe(pkt, true, NULL);
        //TEMP ALWAYS SUCCES FOR NOW
        pkt->result = Packet::Success;
    }
    else
    {
        if (pkt->isResponse())
            handleResponse(pkt);
        else
            return snoopProbe(pkt);
    }
    //Fix this timing info
    return hitLatency;
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::
doFunctionalAccess(PacketPtr pkt, bool isCpuSide)
{
    if (isCpuSide)
    {
        //TEMP USE CPU?THREAD 0 0
        pkt->req->setThreadContext(0,0);

        probe(pkt, false, memSidePort);
        //TEMP ALWAYS SUCCESFUL FOR NOW
        pkt->result = Packet::Success;
    }
    else
    {
            probe(pkt, false, cpuSidePort);
    }
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::
recvStatusChange(Port::Status status, bool isCpuSide)
{

}


template<class TagStore, class Buffering, class Coherence>
Cache<TagStore,Buffering,Coherence>::
Cache(const std::string &_name,
      Cache<TagStore,Buffering,Coherence>::Params &params)
    : BaseCache(_name, params.baseParams),
      prefetchAccess(params.prefetchAccess),
      tags(params.tags), missQueue(params.missQueue),
      coherence(params.coherence), prefetcher(params.prefetcher),
      hitLatency(params.hitLatency)
{
    tags->setCache(this);
    tags->setPrefetcher(prefetcher);
    missQueue->setCache(this);
    missQueue->setPrefetcher(prefetcher);
    coherence->setCache(this);
    prefetcher->setCache(this);
    prefetcher->setTags(tags);
    prefetcher->setBuffer(missQueue);
    invalidateReq = new Request((Addr) NULL, blkSize, 0);
    invalidatePkt = new Packet(invalidateReq, Packet::InvalidateReq, 0);
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::regStats()
{
    BaseCache::regStats();
    tags->regStats(name());
    missQueue->regStats(name());
    coherence->regStats(name());
    prefetcher->regStats(name());
}

template<class TagStore, class Buffering, class Coherence>
bool
Cache<TagStore,Buffering,Coherence>::access(PacketPtr &pkt)
{
//@todo Add back in MemDebug Calls
//    MemDebug::cacheAccess(pkt);
    BlkType *blk = NULL;
    PacketList writebacks;
    int size = blkSize;
    int lat = hitLatency;
    if (prefetchAccess) {
        //We are determining prefetches on access stream, call prefetcher
        prefetcher->handleMiss(pkt, curTick);
    }
    if (!pkt->req->isUncacheable()) {
        blk = tags->handleAccess(pkt, lat, writebacks);
    } else {
        size = pkt->getSize();
    }
    // If this is a block size write/hint (WH64) allocate the block here
    // if the coherence protocol allows it.
    /** @todo make the fast write alloc (wh64) work with coherence. */
    /** @todo Do we want to do fast writes for writebacks as well? */
    if (!blk && pkt->getSize() >= blkSize && coherence->allowFastWrites() &&
        (pkt->cmd == Packet::WriteReq
         || pkt->cmd == Packet::WriteInvalidateReq) ) {
        // not outstanding misses, can do this
        MSHR* outstanding_miss = missQueue->findMSHR(pkt->getAddr());
        if (pkt->cmd == Packet::WriteInvalidateReq || !outstanding_miss) {
            if (outstanding_miss) {
                warn("WriteInv doing a fastallocate"
                     "with an outstanding miss to the same address\n");
            }
            blk = tags->handleFill(NULL, pkt, BlkValid | BlkWritable,
                                   writebacks);
            ++fastWrites;
        }
    }
    while (!writebacks.empty()) {
        missQueue->doWriteback(writebacks.front());
        writebacks.pop_front();
    }
    DPRINTF(Cache, "%s %x %s blk_addr: %x\n", pkt->cmdString(),
            pkt->getAddr() & (((ULL(1))<<48)-1), (blk) ? "hit" : "miss",
            pkt->getAddr() & ~((Addr)blkSize - 1));
    if (blk) {
        // Hit
        hits[pkt->cmdToIndex()][0/*pkt->req->getThreadNum()*/]++;
        // clear dirty bit if write through
        if (pkt->needsResponse())
            respond(pkt, curTick+lat);
        if (pkt->cmd == Packet::Writeback) {
            //Signal that you can kill the pkt/req
            pkt->flags |= SATISFIED;
        }
        return true;
    }

    // Miss
    if (!pkt->req->isUncacheable()) {
        misses[pkt->cmdToIndex()][0/*pkt->req->getThreadNum()*/]++;
        /** @todo Move miss count code into BaseCache */
        if (missCount) {
            --missCount;
            if (missCount == 0)
                exitSimLoop("A cache reached the maximum miss count");
        }
    }

    if (pkt->flags & SATISFIED) {
        // happens when a store conditional fails because it missed
        // the cache completely
        if (pkt->needsResponse())
            respond(pkt, curTick+lat);
    } else {
        missQueue->handleMiss(pkt, size, curTick + hitLatency);
    }

    return true;
}


template<class TagStore, class Buffering, class Coherence>
PacketPtr
Cache<TagStore,Buffering,Coherence>::getPacket()
{
    assert(missQueue->havePending());
    PacketPtr pkt = missQueue->getPacket();
    if (pkt) {
        if (!pkt->req->isUncacheable()) {
            if (pkt->cmd == Packet::HardPFReq)
                misses[Packet::HardPFReq][0/*pkt->req->getThreadNum()*/]++;
            BlkType *blk = tags->findBlock(pkt);
            Packet::Command cmd = coherence->getBusCmd(pkt->cmd,
                                              (blk)? blk->status : 0);
            missQueue->setBusCmd(pkt, cmd);
        }
    }

    assert(!doMasterRequest() || missQueue->havePending());
    assert(!pkt || pkt->time <= curTick);
    return pkt;
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::sendResult(PacketPtr &pkt, MSHR* mshr,
                                                bool success)
{
    if (success && !(pkt && (pkt->flags & NACKED_LINE))) {
        if (!mshr->pkt->needsResponse()
            && !(mshr->pkt->cmd == Packet::UpgradeReq)
            && (pkt && (pkt->flags & SATISFIED))) {
            //Writeback, clean up the non copy version of the packet
            delete pkt;
        }
        missQueue->markInService(mshr->pkt, mshr);
        //Temp Hack for UPGRADES
        if (mshr->pkt && mshr->pkt->cmd == Packet::UpgradeReq) {
            assert(pkt);  //Upgrades need to be fixed
            pkt->flags &= ~CACHE_LINE_FILL;
            BlkType *blk = tags->findBlock(pkt);
            CacheBlk::State old_state = (blk) ? blk->status : 0;
            CacheBlk::State new_state = coherence->getNewState(pkt,old_state);
            if (old_state != new_state)
                DPRINTF(Cache, "Block for blk addr %x moving from "
                        "state %i to %i\n",
                        pkt->getAddr() & (((ULL(1))<<48)-1),
                        old_state, new_state);
            //Set the state on the upgrade
            memcpy(pkt->getPtr<uint8_t>(), blk->data, blkSize);
            PacketList writebacks;
            tags->handleFill(blk, mshr, new_state, writebacks, pkt);
            assert(writebacks.empty());
            missQueue->handleResponse(pkt, curTick + hitLatency);
        }
    } else if (pkt && !pkt->req->isUncacheable()) {
        pkt->flags &= ~NACKED_LINE;
        pkt->flags &= ~SATISFIED;
        pkt->flags &= ~SNOOP_COMMIT;

//Rmove copy from mshr
        delete mshr->pkt;
        mshr->pkt = pkt;

        missQueue->restoreOrigCmd(pkt);
    }
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::handleResponse(PacketPtr &pkt)
{
    BlkType *blk = NULL;
    if (pkt->senderState) {
        //Delete temp copy in MSHR, restore it.
        delete ((MSHR*)pkt->senderState)->pkt;
        ((MSHR*)pkt->senderState)->pkt = pkt;
        if (pkt->result == Packet::Nacked) {
            //pkt->reinitFromRequest();
            warn("NACKs from devices not connected to the same bus "
                 "not implemented\n");
            return;
        }
        if (pkt->result == Packet::BadAddress) {
            //Make the response a Bad address and send it
        }
//	MemDebug::cacheResponse(pkt);
        DPRINTF(Cache, "Handling reponse to %x, blk addr: %x\n",pkt->getAddr(),
                pkt->getAddr() & (((ULL(1))<<48)-1));

        if (pkt->isCacheFill() && !pkt->isNoAllocate()) {
            blk = tags->findBlock(pkt);
            CacheBlk::State old_state = (blk) ? blk->status : 0;
            PacketList writebacks;
            CacheBlk::State new_state = coherence->getNewState(pkt,old_state);
            if (old_state != new_state)
                DPRINTF(Cache, "Block for blk addr %x moving from "
                        "state %i to %i\n",
                        pkt->getAddr() & (((ULL(1))<<48)-1),
                        old_state, new_state);
            blk = tags->handleFill(blk, (MSHR*)pkt->senderState,
                                   new_state, writebacks, pkt);
            while (!writebacks.empty()) {
                    missQueue->doWriteback(writebacks.front());
                    writebacks.pop_front();
            }
        }
        missQueue->handleResponse(pkt, curTick + hitLatency);
    }
}

template<class TagStore, class Buffering, class Coherence>
PacketPtr
Cache<TagStore,Buffering,Coherence>::getCoherencePacket()
{
    return coherence->getPacket();
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::sendCoherenceResult(PacketPtr &pkt,
                                                         MSHR *cshr,
                                                         bool success)
{
    coherence->sendResult(pkt, cshr, success);
}


template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::snoop(PacketPtr &pkt)
{
    if (pkt->req->isUncacheable()) {
        //Can't get a hit on an uncacheable address
        //Revisit this for multi level coherence
        return;
    }

    //Send a timing (true) invalidate up if the protocol calls for it
    coherence->propogateInvalidate(pkt, true);

    Addr blk_addr = pkt->getAddr() & ~(Addr(blkSize-1));
    BlkType *blk = tags->findBlock(pkt);
    MSHR *mshr = missQueue->findMSHR(blk_addr);
    if (coherence->hasProtocol() || pkt->isInvalidate()) {
        //@todo Move this into handle bus req
        //If we find an mshr, and it is in service, we need to NACK or
        //invalidate
        if (mshr) {
            if (mshr->inService) {
                if ((mshr->pkt->isInvalidate() || !mshr->pkt->isCacheFill())
                    && (pkt->cmd != Packet::InvalidateReq
                        && pkt->cmd != Packet::WriteInvalidateReq)) {
                    //If the outstanding request was an invalidate
                    //(upgrade,readex,..)  Then we need to ACK the request
                    //until we get the data Also NACK if the outstanding
                    //request is not a cachefill (writeback)
                    assert(!(pkt->flags & SATISFIED));
                    pkt->flags |= SATISFIED;
                    pkt->flags |= NACKED_LINE;
                    ///@todo NACK's from other levels
                    //warn("NACKs from devices not connected to the same bus "
                    //"not implemented\n");
                    //respondToSnoop(pkt, curTick + hitLatency);
                    return;
                }
                else {
                    //The supplier will be someone else, because we are
                    //waiting for the data.  This should cause this cache to
                    //be forced to go to the shared state, not the exclusive
                    //even though the shared line won't be asserted.  But for
                    //now we will just invlidate ourselves and allow the other
                    //cache to go into the exclusive state.  @todo Make it so
                    //a read to a pending read doesn't invalidate.  @todo Make
                    //it so that a read to a pending read can't be exclusive
                    //now.

                    //Set the address so find match works
                    //panic("Don't have invalidates yet\n");
                    invalidatePkt->addrOverride(pkt->getAddr());

                    //Append the invalidate on
                    missQueue->addTarget(mshr,invalidatePkt);
                    DPRINTF(Cache, "Appending Invalidate to blk_addr: %x\n",
                            pkt->getAddr() & (((ULL(1))<<48)-1));
                    return;
                }
            }
        }
        //We also need to check the writeback buffers and handle those
        std::vector<MSHR *> writebacks;
        if (missQueue->findWrites(blk_addr, writebacks)) {
            DPRINTF(Cache, "Snoop hit in writeback to blk_addr: %x\n",
                    pkt->getAddr() & (((ULL(1))<<48)-1));

            //Look through writebacks for any non-uncachable writes, use that
            for (int i=0; i<writebacks.size(); i++) {
                mshr = writebacks[i];

                if (!mshr->pkt->req->isUncacheable()) {
                    if (pkt->isRead()) {
                        //Only Upgrades don't get here
                        //Supply the data
                        assert(!(pkt->flags & SATISFIED));
                        pkt->flags |= SATISFIED;

                        //If we are in an exclusive protocol, make it ask again
                        //to get write permissions (upgrade), signal shared
                        pkt->flags |= SHARED_LINE;

                        assert(pkt->isRead());
                        Addr offset = pkt->getAddr() & (blkSize - 1);
                        assert(offset < blkSize);
                        assert(pkt->getSize() <= blkSize);
                        assert(offset + pkt->getSize() <=blkSize);
                        memcpy(pkt->getPtr<uint8_t>(), mshr->pkt->getPtr<uint8_t>() + offset, pkt->getSize());

                        respondToSnoop(pkt, curTick + hitLatency);
                    }

                    if (pkt->isInvalidate()) {
                        //This must be an upgrade or other cache will take
                        //ownership
                        missQueue->markInService(mshr->pkt, mshr);
                    }
                    return;
                }
            }
        }
    }
    CacheBlk::State new_state;
    bool satisfy = coherence->handleBusRequest(pkt,blk,mshr, new_state);
    if (satisfy) {
        DPRINTF(Cache, "Cache snooped a %s request for addr %x and "
                "now supplying data, new state is %i\n",
                pkt->cmdString(), blk_addr, new_state);

        tags->handleSnoop(blk, new_state, pkt);
        respondToSnoop(pkt, curTick + hitLatency);
        return;
    }
    if (blk)
        DPRINTF(Cache, "Cache snooped a %s request for addr %x, "
                "new state is %i\n", pkt->cmdString(), blk_addr, new_state);
    tags->handleSnoop(blk, new_state);
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::snoopResponse(PacketPtr &pkt)
{
    //Need to handle the response, if NACKED
    if (pkt->flags & NACKED_LINE) {
        //Need to mark it as not in service, and retry for bus
        assert(0); //Yeah, we saw a NACK come through

        //For now this should never get called, we return false when we see a
        //NACK instead, by doing this we allow the bus_blocked mechanism to
        //handle the retry For now it retrys in just 2 cycles, need to figure
        //out how to change that Eventually we will want to also have success
        //come in as a parameter Need to make sure that we handle the
        //functionality that happens on successufl return of the sendAddr
        //function
    }
}

template<class TagStore, class Buffering, class Coherence>
void
Cache<TagStore,Buffering,Coherence>::invalidateBlk(Addr addr)
{
    tags->invalidateBlk(addr);
}


/**
 * @todo Fix to not assume write allocate
 */
template<class TagStore, class Buffering, class Coherence>
Tick
Cache<TagStore,Buffering,Coherence>::probe(PacketPtr &pkt, bool update,
                                           CachePort* otherSidePort)
{
//    MemDebug::cacheProbe(pkt);
    if (!pkt->req->isUncacheable()) {
        if (pkt->isInvalidate() && !pkt->isRead()
            && !pkt->isWrite()) {
            //Upgrade or Invalidate, satisfy it, don't forward
            DPRINTF(Cache, "%s %x ? blk_addr: %x\n", pkt->cmdString(),
                    pkt->getAddr() & (((ULL(1))<<48)-1),
                    pkt->getAddr() & ~((Addr)blkSize - 1));
            pkt->flags |= SATISFIED;
            return 0;
        }
    }

    if (!update && (pkt->isWrite() || (otherSidePort == cpuSidePort))) {
        // Still need to change data in all locations.
        otherSidePort->sendFunctional(pkt);
        if (pkt->isRead() && pkt->result == Packet::Success)
            return 0;
    }

    PacketList writebacks;
    int lat;
    BlkType *blk = tags->handleAccess(pkt, lat, writebacks, update);

    DPRINTF(Cache, "%s %x %s blk_addr: %x\n", pkt->cmdString(),
            pkt->getAddr() & (((ULL(1))<<48)-1), (blk) ? "hit" : "miss",
            pkt->getAddr() & ~((Addr)blkSize - 1));


    // Need to check for outstanding misses and writes
    Addr blk_addr = pkt->getAddr() & ~(blkSize - 1);

    // There can only be one matching outstanding miss.
    MSHR* mshr = missQueue->findMSHR(blk_addr);

    // There can be many matching outstanding writes.
    std::vector<MSHR*> writes;
    missQueue->findWrites(blk_addr, writes);

    if (!update) {
        // Check for data in MSHR and writebuffer.
        if (mshr) {
            MSHR::TargetList *targets = mshr->getTargetList();
            MSHR::TargetList::iterator i = targets->begin();
            MSHR::TargetList::iterator end = targets->end();
            for (; i != end; ++i) {
                PacketPtr target = *i;
                // If the target contains data, and it overlaps the
                // probed request, need to update data
                if (target->intersect(pkt)) {
                    fixPacket(pkt, target);
                }
            }
        }
        for (int i = 0; i < writes.size(); ++i) {
            PacketPtr write = writes[i]->pkt;
            if (write->intersect(pkt)) {
                fixPacket(pkt, write);
            }
        }
        if (pkt->isRead()
            && pkt->result != Packet::Success
            && otherSidePort == memSidePort) {
            otherSidePort->sendFunctional(pkt);
            assert(pkt->result == Packet::Success);
        }
        return 0;
    } else if (!blk && !(pkt->flags & SATISFIED)) {
        // update the cache state and statistics
        if (mshr || !writes.empty()){
            // Can't handle it, return request unsatisfied.
            panic("Atomic access ran into outstanding MSHR's or WB's!");
        }
        if (!pkt->req->isUncacheable()) {
                // Fetch the cache block to fill
            BlkType *blk = tags->findBlock(pkt);
            Packet::Command temp_cmd = coherence->getBusCmd(pkt->cmd,
                                                            (blk)? blk->status : 0);

            PacketPtr busPkt = new Packet(pkt->req,temp_cmd, -1, blkSize);

            busPkt->allocate();

            busPkt->time = curTick;

            DPRINTF(Cache, "Sending a atomic %s for %x blk_addr: %x\n",
                    busPkt->cmdString(),
                    busPkt->getAddr() & (((ULL(1))<<48)-1),
                    busPkt->getAddr() & ~((Addr)blkSize - 1));

            lat = memSidePort->sendAtomic(busPkt);

            //Be sure to flip the response to a request for coherence
            if (busPkt->needsResponse()) {
                busPkt->makeAtomicResponse();
            }

/*		if (!(busPkt->flags & SATISFIED)) {
// blocked at a higher level, just return
return 0;
}

*/		misses[pkt->cmdToIndex()][0/*pkt->req->getThreadNum()*/]++;

            CacheBlk::State old_state = (blk) ? blk->status : 0;
            CacheBlk::State new_state =
                coherence->getNewState(busPkt, old_state);
            DPRINTF(Cache,
                        "Receive response:%s for blk addr %x in state %i\n",
                    busPkt->cmdString(),
                    busPkt->getAddr() & (((ULL(1))<<48)-1), old_state);
            if (old_state != new_state)
                    DPRINTF(Cache, "Block for blk addr %x moving from "
                            "state %i to %i\n",
                            busPkt->getAddr() & (((ULL(1))<<48)-1),
                            old_state, new_state);

            tags->handleFill(blk, busPkt,
                             new_state,
                             writebacks, pkt);
            //Free the packet
            delete busPkt;

            // Handle writebacks if needed
            while (!writebacks.empty()){
                PacketPtr wbPkt = writebacks.front();
                memSidePort->sendAtomic(wbPkt);
                writebacks.pop_front();
                delete wbPkt;
            }
                return lat + hitLatency;
        } else {
            return memSidePort->sendAtomic(pkt);
        }
    } else {
        if (blk) {
            // There was a cache hit.
            // Handle writebacks if needed
            while (!writebacks.empty()){
                memSidePort->sendAtomic(writebacks.front());
                writebacks.pop_front();
            }

            hits[pkt->cmdToIndex()][0/*pkt->req->getThreadNum()*/]++;
        }

        return hitLatency;
    }

    return 0;
}

template<class TagStore, class Buffering, class Coherence>
Tick
Cache<TagStore,Buffering,Coherence>::snoopProbe(PacketPtr &pkt)
{
    //Send a atomic (false) invalidate up if the protocol calls for it
    coherence->propogateInvalidate(pkt, false);

    Addr blk_addr = pkt->getAddr() & ~(Addr(blkSize-1));
    BlkType *blk = tags->findBlock(pkt);
    MSHR *mshr = missQueue->findMSHR(blk_addr);
    CacheBlk::State new_state = 0;
    bool satisfy = coherence->handleBusRequest(pkt,blk,mshr, new_state);
    if (satisfy) {
        DPRINTF(Cache, "Cache snooped a %s request for addr %x and "
                "now supplying data, new state is %i\n",
                pkt->cmdString(), blk_addr, new_state);

            tags->handleSnoop(blk, new_state, pkt);
            return hitLatency;
    }
    if (blk)
        DPRINTF(Cache, "Cache snooped a %s request for addr %x, "
                "new state is %i\n",
                    pkt->cmdString(), blk_addr, new_state);
    tags->handleSnoop(blk, new_state);
    return 0;
}

