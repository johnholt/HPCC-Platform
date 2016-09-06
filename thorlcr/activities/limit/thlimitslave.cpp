/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "slave.ipp"
#include "eclhelper.hpp"
#include "thactivityutil.ipp"
#include "thbuf.hpp"
#include "thbufdef.hpp"
#include "thormisc.hpp"


class CLimitSlaveActivityBase : public CSlaveActivity
{
    typedef CSlaveActivity PARENT;

protected:
    rowcount_t rowLimit;
    bool eos, eogNext, stopped, resultSent, anyThisGroup;
    IHThorLimitArg *helper;

    void stopInput(rowcount_t c)
    {
        if (!stopped)
        {
            stopped = true;
            sendResult(c);
            PARENT::stop();
        }
    }

public:
    CLimitSlaveActivityBase(CGraphElementBase *_container) : CSlaveActivity(_container)
    {
        helper = (IHThorLimitArg *)queryHelper();
        resultSent = true; // unless started suppress result send
        eos = stopped = anyThisGroup = eogNext = false;
        rowLimit = RCMAX;
        if (container.queryLocal())
            setRequireInitData(false);
        appendOutputLinked(this);
    }
    virtual void init(MemoryBuffer &data, MemoryBuffer &slaveData) override
    {
        if (!container.queryLocal())
            mpTag = container.queryJobChannel().deserializeMPTag(data);
    }
    virtual void start() override
    {
        ActivityTimer s(totalCycles, timeActivities);
        PARENT::start();
        resultSent = container.queryLocal(); // i.e. local, so don't send result to master
        eos = stopped = anyThisGroup = eogNext = false;
        rowLimit = (rowcount_t)helper->getRowLimit();
    }
    void sendResult(rowcount_t r)
    {
        if (resultSent) return;
        resultSent = true;
        CMessageBuffer mb;
        mb.append(r);
        queryJobChannel().queryJobComm().send(mb, 0, mpTag);
    }
    virtual void stop() override
    {
        stopInput(getDataLinkCount());
    }
    virtual bool isGrouped() const override { return queryInput(0)->isGrouped(); }
    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) override
    {
        initMetaInfo(info);
        info.canReduceNumRows = true;
        info.canBufferInput = false;
        info.totalRowsMax = rowLimit;
        calcMetaInfoSize(info, queryInput(0));
    }
};

class CLimitSlaveActivity : public CLimitSlaveActivityBase, public CThorSteppable
{
public:

    CLimitSlaveActivity(CGraphElementBase *container) : CLimitSlaveActivityBase(container), CThorSteppable(this) { }
    
    CATCH_NEXTROW()
    {
        ActivityTimer t(totalCycles, timeActivities);
        if (eos)
            return NULL;
        while (!abortSoon && !eogNext)
        {
            OwnedConstThorRow row = inputStream->nextRow();
            if (!row)
            {
                if(anyThisGroup)
                {
                    anyThisGroup = false;
                    break;
                }
                row.setown(inputStream->nextRow());
                if (!row)
                {
                    eos = true;
                    sendResult(getDataLinkCount());
                    break;
                }
            }
            anyThisGroup = true;
            if (getDataLinkCount() >= rowLimit)
            {
                eos = true;
                sendResult(getDataLinkCount());
                helper->onLimitExceeded();
                return NULL;
            }
            dataLinkIncrement();
            return row.getClear();
        }
        eogNext = false;
        return NULL;
    }
    const void *nextRowGE(const void *seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
    {
        try { return nextRowGENoCatch(seek, numFields, wasCompleteMatch, stepExtra); }
        CATCH_NEXTROWX_CATCH;
    }
    const void *nextRowGENoCatch(const void *seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
    {
        ActivityTimer t(totalCycles, timeActivities);
        OwnedConstThorRow ret = inputStream->nextRowGE(seek, numFields, wasCompleteMatch, stepExtra);
        if (ret)
        {
            if (wasCompleteMatch)
                dataLinkIncrement();
            if (getDataLinkCount() > rowLimit)
                helper->onLimitExceeded();
        }
        return ret.getClear();
    }
    bool gatherConjunctions(ISteppedConjunctionCollector &collector) 
    { 
        return input->gatherConjunctions(collector); 
    }
    void resetEOF() 
    { 
        //Do not reset the rowLimit
        inputStream->resetEOF();
    }
// steppable
    virtual void setInputStream(unsigned index, CThorInput &input, bool consumerOrdered) override
    {
        CLimitSlaveActivityBase::setInputStream(index, input, consumerOrdered);
        CThorSteppable::setInputStream(index, input, consumerOrdered);
    }
    virtual IInputSteppingMeta *querySteppingMeta() { return CThorSteppable::inputStepping; }
};

class CSkipLimitSlaveActivity : public CLimitSlaveActivityBase
{
    UnsignedArray sizeArray;
    size32_t ptrIndex;
    bool limitChecked, eof, rowTransform;
    IHThorLimitTransformExtra *helperex;
    Owned<IRowWriterMultiReader> buf;
    Owned<IRowStream> reader;

    rowcount_t getResult()
    {
        assertex(resultSent);
        CMessageBuffer mb;
        if (!receiveMsg(mb, 0, mpTag))
            return RCUNSET;
        rowcount_t r;
        mb.read(r);
        return r;
    }

    bool gather()
    {
        rowcount_t count = 0;
        while (!abortSoon)
        {
            OwnedConstThorRow row = inputStream->nextRow();
            if (!row)
            {
                row.setown(inputStream->nextRow());
                if (!row)
                    break;
                else
                    buf->putRow(NULL); 
            }
            buf->putRow(row.getClear());
            if (++count > rowLimit)
                break;

            // We used to warn if excessive buffering. I think there should be callback to signal,
            // Alternatively, could do via IDiskUsage whish smart buffer used to take...
            //throw MakeActivityException(this, 0, "SkipLimit(%" ACTPF "d) exceeded activity buffering limit", container.queryId());
        }
        buf->flush();
        stopInput(count);
        rowcount_t total = container.queryLocal() ? count : getResult();
        if (total > rowLimit)
        {
            buf.clear();
            return false;
        }
        else
        {
            reader.setown(buf->getReader());
            buf.clear();
            return true;
        }
    }

public:
    CSkipLimitSlaveActivity(CGraphElementBase *container,bool _rowTransform) : CLimitSlaveActivityBase(container)
    {
        ptrIndex = 0;
        limitChecked = eof = false;
        rowTransform = _rowTransform;
        helperex = NULL;
        if (rowTransform)
            helperex = static_cast<IHThorLimitTransformExtra *>(queryHelper()->selectInterface(TAIlimittransformextra_1));
    }
    void abort()
    {
        if (!container.queryLocal())
            cancelReceiveMsg(0, mpTag);
        CLimitSlaveActivityBase::abort();
    }
    virtual void start() override
    {
        CLimitSlaveActivityBase::start();
        buf.setown(createOverflowableBuffer(*this, this, true));
    }
    CATCH_NEXTROW()
    {
        ActivityTimer t(totalCycles, timeActivities);
        if (eof) 
            return NULL;
        if (!limitChecked)
        {
            limitChecked = true;
            if (!gather())
            {
                eof = true;
                if (rowTransform&&helperex && firstNode())
                {
                    try
                    {
                        RtlDynamicRowBuilder ret(queryRowAllocator());
                        size32_t sizeGot = helperex->transformOnLimitExceeded(ret);
                        if (sizeGot)
                        {
                            dataLinkIncrement();
                            return ret.finalizeRowClear(sizeGot);
                        }
                    }
                    catch (IException *e)
                    {
                        ActPrintLog(e, "In transformOnLimitExceeded");
                        throw; 
                    }
                    catch (CATCHALL)
                    { 
                        ActPrintLog("LIMIT: Unknown exception in transformOnLimitExceeded()"); 
                        throw;
                    }
                }
                return NULL;
            }
        }
        OwnedConstThorRow row = reader->nextRow();
        if (row)
        {
            dataLinkIncrement();
            anyThisGroup = true;
            return row.getClear();
        }
        if (anyThisGroup)
            anyThisGroup = false;
        else
            eof = true;
        return NULL;
    }
};

activityslaves_decl CActivityBase *createLimitSlave(CGraphElementBase *container)
{
    return new CLimitSlaveActivity(container);
}


activityslaves_decl CActivityBase *createSkipLimitSlave(CGraphElementBase *container)
{
    return new CSkipLimitSlaveActivity(container,false);
}

activityslaves_decl CActivityBase *createRowLimitSlave(CGraphElementBase *container)
{
    return new CSkipLimitSlaveActivity(container,true);
}

