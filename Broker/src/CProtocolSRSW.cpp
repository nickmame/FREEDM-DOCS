////////////////////////////////////////////////////////////////////////////////
/// @file         CProtocolSRSW.cpp
///
/// @author       Stephen Jackson <scj7t4@mst.edu>
///
/// @project      FREEDM DGI
///
/// @description  Declare CProtocolSRSW class
///
/// These source code files were created at Missouri University of Science and
/// Technology, and are intended for use in teaching or research. They may be
/// freely copied, modified, and redistributed as long as modified versions are
/// clearly marked as such and this notice is not removed. Neither the authors
/// nor Missouri S&T make any warranty, express or implied, nor assume any legal
/// responsibility for the accuracy, completeness, or usefulness of these files
/// or any information distributed with these files.
///
/// Suggested modifications or questions about these files can be directed to
/// Dr. Bruce McMillin, Department of Computer Science, Missouri University of
/// Science and Technology, Rolla, MO 65409 <ff@mst.edu>.
////////////////////////////////////////////////////////////////////////////////

#include "CProtocolSRSW.hpp"

#include "CConnection.hpp"
#include "CLogger.hpp"
#include "CBroker.hpp"
#include "messages/ModuleMessage.pb.h"
#include "messages/ProtocolMessage.pb.h"

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace freedm {
    namespace broker {

namespace {

/// This file's logger.
CLocalLogger Logger(__FILE__);

}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::CProtocolSRSW
/// @description Constructor for the CProtocolSRSW class.
/// @pre The object is uninitialized.
/// @post The object is initialized: m_killwindow is empty, the connection is
///       marked as unsynced, It won't be sending kill statuses. Its first
///       message will be numbered as 0 for outgoing and the timer is not set.
/// @param uuid The peer this connection is to.
///////////////////////////////////////////////////////////////////////////////
CProtocolSRSW::CProtocolSRSW(std::string uuid,boost::asio::ip::udp::endpoint endpoint)
    : IProtocol(uuid,endpoint),
      m_timeout(CBroker::Instance().GetIOService())
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    //Sequence Numbers
    m_outseq = 0;
    m_inseq = 0;
    //Inbound Message Sequencing
    m_insync = false;
    m_inresyncs = 0;
    //Outbound message sequencing
    m_outsync = false;
}

void CProtocolSRSW::ChangePhase(bool /*newround*/)
{
    //m_outseq = 0;
    m_outsync = false;
    m_window.clear();
    m_outstandingwindow.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::CProtocolSRSW
/// @description Send function for the CProtocolSRSW. Sending using this
///   protocol involves an alternating bit scheme. Messages can expire and
///   delivery won't be attempted after the deadline is passed. Killed messages
///   are noted in the next outgoing message. The receiver tracks the killed
///   messages and uses them to help maintain ordering.
/// @pre The protocol is intialized.
/// @post At least one message is in the channel and actively being resent.
///     The send window is greater than or equal to one. The timer for the
///     resend is freshly set or is currently running for a resend.
///     If a message is written to the channel, the m_killable flag is set.
/// @param msg The message to write to the channel.
///////////////////////////////////////////////////////////////////////////////
void CProtocolSRSW::Send(const ModuleMessage& msg)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;

    if(m_outsync == false)
    {
        SendSYN();
    }

    ProtocolMessage pm;
    pm.mutable_module_message()->CopyFrom(msg);

    pm.set_sequence_num(m_outseq);
    m_outseq = (m_outseq+1) % SEQUENCE_MODULO;

    if(m_outstandingwindow.size() < OUTSTANDING_WINDOW)
    {
        Write(pm);
        m_outstandingwindow.push_back(pm);
        boost::system::error_code x;
        Resend(x);
    }
    else
    {
        m_window.push_back(pm);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::CProtocolSRSW
/// @description Handles refiring ACKs and Sent Messages.
/// @pre The connection has received or sent at least one message.
/// @post One of the following conditions or combination of states is
///       upheld:
///       1) An ack for a message that has not yet expired has been resent and
///          a timer to call resend has been set.
///       2) Message(s) has/have expired and are removed from the queue. The
///          flag to send kills is set.
///       3) The window is empty and no message is set to the channel, the
///          timer is not re-set.
///       4) A message expired and then next message will cause the sequence
///          numbers to wrap, (or they have wrapped since the last time a message
///          was successfully sent) so a sync is inserted at the front of the queue
///          to skip that case on the receiver side. The sendkill flag is cleared
///          and the sendkill value is cleared.
///       5) If there is still a message to resend, the timer is reset.
/// @param err The timer error code. If the err is 0 then the timer expired
///////////////////////////////////////////////////////////////////////////////
void CProtocolSRSW::Resend(const boost::system::error_code& err)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    if(!err && !GetStopped())
    {
        if(m_outstandingwindow.size() > 0)
        {
            Logger.Trace<<__PRETTY_FUNCTION__<<" Writing"<<std::endl;
            std::deque<ProtocolMessage>::iterator it;
            for(it = m_outstandingwindow.begin(); it != m_outstandingwindow.end(); it++)
            {
                Write(*it);
            }
            // Head of window can be killed.
            m_timeout.cancel();
            m_timeout.expires_from_now(boost::posix_time::milliseconds(REFIRE_TIME));
            m_timeout.async_wait(boost::bind(&CProtocolSRSW::Resend,
                boost::static_pointer_cast<CProtocolSRSW>(shared_from_this()),
                boost::asio::placeholders::error));
        }
    }
    Logger.Trace<<__PRETTY_FUNCTION__<<" Resend Finished"<<std::endl;
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::ReceiveACK
/// @description Marks a message as acknowledged by the receiver and moves to
///     transmit the next message.
/// @pre A message has been sent.
/// @post If the ACK corresponds to the head of window by a match of sequence
///       number, the the message is popped.
///       If the there is still an message in the window to send, the
///       resend function is called.
///////////////////////////////////////////////////////////////////////////////
void CProtocolSRSW::ReceiveACK(const ProtocolMessage& msg)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    unsigned int seq = msg.sequence_num();
    while(m_outstandingwindow.size() > 0)
    {
        unsigned int fseq = m_outstandingwindow.front().sequence_num();
        unsigned int bounda = fseq;
        unsigned int boundb = (fseq+OUTSTANDING_WINDOW)%SEQUENCE_MODULO;
        Logger.Debug<<"Received ACK "<<seq<<" expecting ACK "<<fseq<<std::endl;
        if(bounda <= seq || (seq < boundb and boundb < bounda))
        {
            m_outstandingwindow.pop_front();
            if(m_window.size() > 0)
            {
                m_outstandingwindow.push_back(m_window.front());
                m_window.pop_front();
            }
        }
        else
        {
            break;
        }
    }
    if(m_outstandingwindow.size() > 0)
    {
        boost::system::error_code x;
        Resend(x);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::Receive
/// @description Accepts a message into the protocol, if that message should
///   be accepted. If this function returns true, the message is passed to
///   the dispatcher. Since this message accepts SYNs there might be times
///   when processing and state changes but the message is marked as "rejected"
///   this is normal.
/// @pre Accept logic can be complicated, there are several scenarios that
///      should be addressed.
///      1) A bad request has been received
///      2) A SYN message is received for the first time
///      3) A SYN message is received as a duplicate.
///      4) A Message has been received before the connection has been synced.
///      5) A Message has been received with the expected sequenceno with or
///         without a kill flag.
///      6) A message has been received with a kill flag. The kill is greater
///         than the expected sequence number
///      7) A message has been received with a kill flag. The kill is less than
///         the expected sequence number. However, the message's number is less
///         than the expected sequence number
///      8) A message has been received with a kill flag. The kill is less than
///         the expected sequence number and the message's sequence number is
///         greater than the expected sequence number.
/// @post Cases are handled as follows:
///      1) The connection is resynced.
///      2) The message is ACKed, the send time of the sync is noted, and the
///         connection is synced.
///      3) The SYN is ignored.
///      4) A bad request message is generated and sent to the source.
///      5) The message is accepted.
///      6) The message is rejected. Kills should only ever be less than the
///         expected sequence number unless the message is arrived out of order
///      7) The message is simply old but still arriving at the socket, and can
///         be rejected.
///      8) The message should be accepted because one or more message expired
///         in the gap of sequence numbers.
/// @return True if the message is accepted, false otherwise.
///////////////////////////////////////////////////////////////////////////////
bool CProtocolSRSW::Receive(const ProtocolMessage& msg)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    boost::posix_time::ptime sendtime = boost::posix_time::time_from_string(msg.send_time());
    if(msg.status() == ProtocolMessage::BAD_REQUEST)
    {
        //See if we are already trying to sync:
        if(m_window.front().status() != ProtocolMessage::CREATED)
        {
            if(m_outsynctime != sendtime)
            {
                Logger.Debug<<"Syncronizing Connection (BAD REQUEST)"<<std::endl;
                m_outsynctime = sendtime;
                SendSYN();
            }
            else
            {
                Logger.Debug<<"Already synced for this time"<<std::endl;
            }
        }
        return false;
    }
    if(msg.status() == ProtocolMessage::CREATED)
    {
        //Check to see if we've already seen this SYN:
        if(sendtime == m_insynctime)
        {
            return false;
            Logger.Debug<<"Duplicate Sync"<<std::endl;
        }
        Logger.Debug<<"Got Sync"<<std::endl;
        m_inseq = (msg.sequence_num()+1)%SEQUENCE_MODULO;
        m_insynctime = sendtime;
        m_inresyncs++;
        m_insync = true;
        SendACK(msg);
        return false;
    }
    if(m_insync == false)
    {
        Logger.Debug<<"Connection Needs Resync"<<std::endl;
        //If the connection hasn't been synchronized, we want to
        //tell them it is a bad request so they know they need to sync.
        ProtocolMessage outmsg;
        // Presumably, if we are here, the connection is registered
        outmsg.set_status(ProtocolMessage::BAD_REQUEST);
        outmsg.set_sequence_num(m_inresyncs%SEQUENCE_MODULO);
        Write(outmsg);
        return false;
    }
    // Consider the window you expect to see
    unsigned int seq = msg.sequence_num();
    if(m_inseq == seq)
    {
        m_inseq = (seq+1)%SEQUENCE_MODULO;
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::SendACK
/// @description Composes an ack and writes it to the channel. ACKS are saved
///     to the protocol's state and are written again during resends to try and
///     maximize througput.
/// @param msg The message to ACK.
/// @pre A message has been accepted.
/// @post The m_currentack member is set to the ack and the message will
///     be resent during resend until it expires.
///////////////////////////////////////////////////////////////////////////////
void CProtocolSRSW::SendACK(const ProtocolMessage& msg)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    unsigned int seq = msg.sequence_num();
    ProtocolMessage outmsg;
    // Presumably, if we are here, the connection is registered
    outmsg.set_status(ProtocolMessage::ACCEPTED);
    outmsg.set_sequence_num(seq);
    Write(outmsg);
    m_ackmutex.lock();
    m_currentack = outmsg;
    m_ackmutex.unlock();
    /// Hook into resend until the message expires.
    m_timeout.cancel();
    m_timeout.expires_from_now(boost::posix_time::milliseconds(REFIRE_TIME));
    m_timeout.async_wait(boost::bind(&CProtocolSRSW::Resend,
        boost::static_pointer_cast<CProtocolSRSW>(shared_from_this()),
        boost::asio::placeholders::error));
}

///////////////////////////////////////////////////////////////////////////////
/// CProtocolSRSW::SendSYN
/// @description Composes an SYN and writes it to the channel.
/// @pre A message has been accepted.
/// @post A syn has been written to the channel
///////////////////////////////////////////////////////////////////////////////
void CProtocolSRSW::SendSYN()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    unsigned int seq = m_outseq;
    ProtocolMessage outmsg;
    if(m_outstandingwindow.size() == 0)
    {
        m_outseq = (m_outseq+1)%SEQUENCE_MODULO;
    }
    else
    {
        //Don't bother if front of queue is already a SYN
        if(m_outstandingwindow.front().status() == ProtocolMessage::CREATED)
        {
            return;
        }
        //Set it as the seq before the front of queue
        seq = m_outstandingwindow.front().sequence_num();
        if(seq == 0)
        {
            seq = SEQUENCE_MODULO-1;
        }
        else
        {
            seq--;
        }
    }
    // Presumably, if we are here, the connection is registered
    outmsg.set_status(ProtocolMessage::CREATED);
    outmsg.set_sequence_num(seq);
    Write(outmsg);
    m_outstandingwindow.push_front(outmsg);
    m_outsync = true;
    /// Hook into resend until the message expires.
    boost::system::error_code x;
    Resend(x);
}

    }
}
