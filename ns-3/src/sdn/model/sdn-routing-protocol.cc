/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2016 Haoliang Chen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Haoliang Chen <chl41993@gmail.com>
 */


///
/// \brief Implementation of SDN agent on car side 
/// and related classes.
///
/// This is the main file of this software because SDN's behaviour is
/// implemented here.
///

#define NS_LOG_APPEND_CONTEXT                                   \
  if (GetObject<Node> ()) { std::clog << "[node " << GetObject<Node> ()->GetId () << "] "; }


#include "sdn-routing-protocol.h"
#include "ns3/socket-factory.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-routing-table-entry.h"
#include "ns3/ipv4-route.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/ipv4-header.h"

#include "stdlib.h" //ABS

/********** Useful macros **********/

///
/// \brief Gets the delay between a given time and the current time.
///
/// If given time is previous to the current one, then this macro returns
/// a number close to 0. This is used for scheduling events at a certain moment.
///
#define DELAY(time) (((time) < (Simulator::Now ())) ? Seconds (0.000001) : \
                     (time - Simulator::Now () + Seconds (0.000001)))






/********** Miscellaneous constants **********/

/// Maximum allowed jitter.
#define SDN_MAXJITTER          (m_helloInterval.GetSeconds () / 4)
/// Random number between [0-SDN_MAXJITTER] used to jitter SDN packet transmission.
#define JITTER (Seconds (m_uniformRandomVariable->GetValue (0, SDN_MAXJITTER)))


#define SDN_MAX_SEQ_NUM        65535


#define SDN_PORT_NUMBER 419
/// Maximum number of messages per packet.
#define SDN_MAX_MSGS    64


#define INFHOP 2147483647

namespace ns3 {
namespace sdn {

NS_LOG_COMPONENT_DEFINE ("SdnRoutingProtocol");


/********** SDN controller class **********/

NS_OBJECT_ENSURE_REGISTERED (RoutingProtocol);

TypeId
RoutingProtocol::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::sdn::RoutingProtocol")
    .SetParent<Ipv4RoutingProtocol> ()
    .AddConstructor<RoutingProtocol> ();
  return tid;
}


RoutingProtocol::RoutingProtocol ()
  :
    m_packetSequenceNumber (SDN_MAX_SEQ_NUM),
    m_messageSequenceNumber (SDN_MAX_SEQ_NUM),
    m_helloInterval (Seconds(1)),
    m_rmInterval (Seconds (2)),
    m_minAPInterval (Seconds (1)),
    m_ipv4 (0),
    m_helloTimer (Timer::CANCEL_ON_DESTROY),
    m_rmTimer (Timer::CANCEL_ON_DESTROY),
    m_apTimer (Timer::CANCEL_ON_DESTROY),
    m_queuedMessagesTimer (Timer::CANCEL_ON_DESTROY),
    m_SCHinterface (0),
    m_CCHinterface (0),
    m_nodetype (OTHERS),
    m_appointmentResult (NORMAL),
    m_next_forwarder (uint32_t (0)),
    m_linkEstablished (false),
    m_numArea (0),
    m_isPadding (false),
    m_numAreaVaild (false),
    m_road_length (814),//MagicNumber
    m_signal_range (419)
{
  m_uniformRandomVariable = CreateObject<UniformRandomVariable> ();
}

RoutingProtocol::~RoutingProtocol ()
{
  
}

void
RoutingProtocol::SetIpv4 (Ptr<Ipv4> ipv4)
{
  NS_ASSERT (ipv4 != 0);
  NS_ASSERT (m_ipv4 == 0);
  NS_LOG_DEBUG ("Created sdn::RoutingProtocol");
  m_helloTimer.SetFunction 
    (&RoutingProtocol::HelloTimerExpire, this);
  m_queuedMessagesTimer.SetFunction 
    (&RoutingProtocol::SendQueuedMessages, this);
  m_rmTimer.SetFunction
    (&RoutingProtocol::RmTimerExpire, this);
  m_apTimer.SetFunction
    (&RoutingProtocol::APTimerExpire, this);

  m_packetSequenceNumber = SDN_MAX_SEQ_NUM;
  m_messageSequenceNumber = SDN_MAX_SEQ_NUM;


  m_ipv4 = ipv4;
}

void RoutingProtocol::DoDispose ()
{
  m_ipv4 = 0;

  for (std::map< Ptr<Socket>, Ipv4InterfaceAddress >::iterator iter = 
       m_socketAddresses.begin ();
       iter != m_socketAddresses.end (); ++iter)
    {
      iter->first->Close ();
    }
  m_socketAddresses.clear ();
  m_table.clear();

  Ipv4RoutingProtocol::DoDispose ();
}

void
RoutingProtocol::PrintRoutingTable (Ptr<OutputStreamWrapper> stream) const
{
  std::ostream* os = stream->GetStream ();
  *os << "Destination\t\tMask\t\tNextHop\t\tInterface\tDistance\n";

  for (std::map<Ipv4Address, RoutingTableEntry>::const_iterator iter = 
       m_table.begin ();
       iter != m_table.end (); ++iter)
    {
      *os << iter->first << "\t\t";
      *os << iter->second.mask << "\t\t";
      *os << iter->second.nextHop << "\t\t";
      if (Names::FindName (m_ipv4->GetNetDevice (iter->second.interface)) != "")
        {
          *os << 
          Names::FindName (m_ipv4->GetNetDevice (iter->second.interface)) << 
          "\t\t";
        }
      else
        {
          *os << iter->second.interface << "\t\t";
        }
      *os << "\n";
    }
}

void 
RoutingProtocol::DoInitialize ()
{
  if (m_mainAddress == Ipv4Address ())
    {
      Ipv4Address loopback ("127.0.0.1");
      uint32_t count = 0;
      for (uint32_t i = 0; i < m_ipv4->GetNInterfaces (); ++i)
        {
          // CAR Use first address as ID
          // LC Use secend address as ID
          Ipv4Address addr = m_ipv4->GetAddress (i, 0).GetLocal ();
          if (addr != loopback)
            {
              if (m_nodetype == CAR)
                {
                  m_mainAddress = addr;
                  break;
                }
              else
                if (m_nodetype == LOCAL_CONTROLLER)
                  {
                    if (count == 1)
                      {
                        m_mainAddress = addr;
                        break;
                      }
                    ++count;
                  }
            }
        }

      NS_ASSERT (m_mainAddress != Ipv4Address ());
    }

  NS_LOG_DEBUG ("Starting SDN on node " << m_mainAddress);

  Ipv4Address loopback ("127.0.0.1");

  bool canRunSdn = false;
  //Install RecvSDN  Only on CCH channel.
  if(m_interfaceExclusions.find (m_CCHinterface) == m_interfaceExclusions.end ())
    {
      // Create a socket to listen only on this interface
      Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
                                                 UdpSocketFactory::GetTypeId ());
      // TRUE
      socket->SetAllowBroadcast (true);
      InetSocketAddress
        inetAddr (m_ipv4->GetAddress (m_CCHinterface, 0).GetLocal (), SDN_PORT_NUMBER);
      socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvSDN,  this));
      if (socket->Bind (inetAddr))
        {
          NS_FATAL_ERROR ("Failed to bind() SDN socket");
        }
      socket->BindToNetDevice (m_ipv4->GetNetDevice (m_CCHinterface));
      m_socketAddresses[socket] = m_ipv4->GetAddress (m_CCHinterface, 0);

      canRunSdn = true;
    }

  Init_NumArea();
  if(canRunSdn)
    {
      HelloTimerExpire ();
      RmTimerExpire ();
      APTimerExpire ();
      NS_LOG_DEBUG ("SDN on node (Car) " << m_mainAddress << " started");
    }
}

void 
RoutingProtocol::SetCCHInterface (uint32_t interface)
{
  //std::cout<<"SetCCHInterface "<<interface<<std::endl;
  m_mainAddress = m_ipv4->GetAddress (interface, 0).GetLocal ();
  m_CCHinterface = interface;
  Ipv4InterfaceAddress temp_if_add = m_ipv4->GetAddress (m_CCHinterface, 0);
  AddEntry (temp_if_add.GetLocal (),
            Ipv4Address (temp_if_add.GetMask ().Get ()),
            temp_if_add.GetLocal (),
            m_CCHinterface);
  //std::cout<<"SetCCHInterface "<<m_mainAddress.Get ()%256<<std::endl;
}

void 
RoutingProtocol::SetSCHInterface (uint32_t interface)
{
  //std::cout<<"SetSCHInterface "<<interface<<std::endl;
  m_SCHinterface = interface;
  Ipv4InterfaceAddress temp_if_add = m_ipv4->GetAddress (m_SCHinterface, 0);
  AddEntry (temp_if_add.GetLocal (),
            Ipv4Address (temp_if_add.GetMask ().Get ()),
            temp_if_add.GetLocal (),
            m_SCHinterface);
  //std::cout<<"SetSCHInterface "<<m_mainAddress.Get ()%256<<std::endl;
}

void
RoutingProtocol::SetInterfaceExclusions (std::set<uint32_t> exceptions)
{
  m_interfaceExclusions = exceptions;
}

//
// \brief Processes an incoming %SDN packet (Car Side).
void
RoutingProtocol::RecvSDN (Ptr<Socket> socket)
{
  //if (m_mainAddress.Get () % 256 > 50)
  //  std::cout<<"RecvSDN"<<m_mainAddress.Get () % 256<<std::endl;
  Ptr<Packet> receivedPacket;
  Address sourceAddress;
  receivedPacket = socket->RecvFrom (sourceAddress);

  InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom (sourceAddress);
  Ipv4Address senderIfaceAddr = inetSourceAddr.GetIpv4 ();
  Ipv4Address receiverIfaceAddr = m_socketAddresses[socket].GetLocal ();
  NS_ASSERT (receiverIfaceAddr != Ipv4Address ());
  NS_LOG_DEBUG ("SDN node " << m_mainAddress
                << " received a SDN packet from "
                << senderIfaceAddr << " to " << receiverIfaceAddr);

  // All routing messages are sent from and to port RT_PORT,
  // so we check it.
  NS_ASSERT (inetSourceAddr.GetPort () == SDN_PORT_NUMBER);

  Ptr<Packet> packet = receivedPacket;

  sdn::PacketHeader sdnPacketHeader;
  packet->RemoveHeader (sdnPacketHeader);
  NS_ASSERT (sdnPacketHeader.GetPacketLength () >= sdnPacketHeader.GetSerializedSize ());
  uint32_t sizeLeft = sdnPacketHeader.GetPacketLength () - sdnPacketHeader.GetSerializedSize ();

  MessageList messages;

  while (sizeLeft)
    {
      MessageHeader messageHeader;
      if (packet->RemoveHeader (messageHeader) == 0)
        NS_ASSERT (false);

      sizeLeft -= messageHeader.GetSerializedSize ();

      NS_LOG_DEBUG ("SDN Msg received with type "
                    << std::dec << int (messageHeader.GetMessageType ())
                    << " TTL=" << int (messageHeader.GetTimeToLive ())
                    << " SeqNum=" << messageHeader.GetMessageSequenceNumber ());
      messages.push_back (messageHeader);
    }

  m_rxPacketTrace (sdnPacketHeader, messages);
  
  for (MessageList::const_iterator messageIter = messages.begin ();
       messageIter != messages.end (); ++messageIter)
    {
      const MessageHeader &messageHeader = *messageIter;
      // If ttl is less than or equal to zero, or
      // the receiver is the same as the originator,
      // the message must be silently dropped
      if ((messageHeader.GetTimeToLive () == 0)||(IsMyOwnAddress (sdnPacketHeader.originator)))
        {
          // ignore it
          packet->RemoveAtStart (messageHeader.GetSerializedSize ());
          continue;
        }


      switch (messageHeader.GetMessageType ())
        {
        case sdn::MessageHeader::ROUTING_MESSAGE:
          NS_LOG_DEBUG (Simulator::Now ().GetSeconds ()
                        << "s SDN node " << m_mainAddress
                        << " received Routing message of size " 
                        << messageHeader.GetSerializedSize ());
          //Controller Node should discare Hello_Message
          if (GetType() == CAR)
            ProcessRm (messageHeader);
          break;

        case sdn::MessageHeader::HELLO_MESSAGE:
          NS_LOG_DEBUG (Simulator::Now ().GetSeconds ()
                        << "s SDN node " << m_mainAddress
                        << " received Routing message of size "
                        << messageHeader.GetSerializedSize ());
          //Car Node should discare Hello_Message
          if (GetType() == LOCAL_CONTROLLER)
            ProcessHM (messageHeader);
          break;

        case sdn::MessageHeader::APPOINTMENT_MESSAGE:
          NS_LOG_DEBUG (Simulator::Now ().GetSeconds ()
                        << "s SDN node " << m_mainAddress
                        << " received Appointment message of size "
                        << messageHeader.GetSerializedSize ());
          if (GetType() == CAR)
            ProcessAppointment (messageHeader);
          break;

        default:
          NS_LOG_DEBUG ("SDN message type " <<
                        int (messageHeader.GetMessageType ()) <<
                        " not implemented");
        }

    }
    
}// End of RecvSDN

void
RoutingProtocol::ProcessHM (const sdn::MessageHeader &msg)
{
  /*std::cout<<m_mainAddress.Get ()%256<<" RoutingProtocol::ProcessHM "
      <<msg.GetHello ().ID.Get ()%256<<" Speed:"
      <<msg.GetHello ().GetVelocity ().x<<std::endl;
  */
  Ipv4Address ID = msg.GetHello ().ID;
  std::map<Ipv4Address, CarInfo>::iterator it = m_lc_info.find (ID);

  if (it != m_lc_info.end ())
    {
      it->second.Active = true;
      it->second.LastActive = Simulator::Now ();
      it->second.Position = msg.GetHello ().GetPosition ();
      it->second.Velocity = msg.GetHello ().GetVelocity ();
    }
  else
    {
      CarInfo CI_temp;
      CI_temp.Active = true;
      CI_temp.LastActive = Simulator::Now ();
      CI_temp.Position = msg.GetHello ().GetPosition ();
      CI_temp.Velocity = msg.GetHello ().GetVelocity ();
      m_lc_info[ID] = CI_temp;
    }
  //std::cout<<"V:"<<m_lc_info[ID].Velocity.x<<std::endl;
}

// \brief Build routing table according to Rm
void
RoutingProtocol::ProcessRm (const sdn::MessageHeader &msg)
{
  NS_LOG_FUNCTION (msg);
  
  const sdn::MessageHeader::Rm &rm = msg.GetRm();
  // Check if this rm is for me
  // Ignore rm that ID does not match.
  if (IsMyOwnAddress (rm.ID))
    {
      Time now = Simulator::Now();
      NS_LOG_DEBUG ("@" << now.GetSeconds() << ":Node " << m_mainAddress
                    << "ProcessRm.");

      NS_ASSERT (rm.GetRoutingMessageSize() >= 0);

      Clear();

      for (std::vector<sdn::MessageHeader::Rm::Routing_Tuple>::const_iterator it = rm.routingTables.begin();
            it != rm.routingTables.end();
            ++it)
      {

        AddEntry(it->destAddress,
                 it->mask,
                 it->nextHop,
                 0);
      }
    }
}

void
RoutingProtocol::ProcessAppointment (const sdn::MessageHeader &msg)
{
  NS_LOG_FUNCTION (msg);
  const sdn::MessageHeader::Appointment &appointment = msg.GetAppointment ();
  if (IsMyOwnAddress (appointment.ID))
    {
      switch (appointment.ATField)
      {
        case NORMAL:
          //std::cout<<" \"NORMAL\""<<std::endl;
          break;
        case FORWARDER:
          m_next_forwarder = appointment.NextForwarder;
          //std::cout<<"CAR"<<m_mainAddress.Get () % 256<<"ProcessAppointment";
          //std::cout<<" \"FORWARDER\""<<std::endl;
          //std::cout<<"NextForwarder:"<<m_next_forwarder.Get () % 256<<std::endl;
          break;
        default:
          std::cout<<" ERROR TYPE"<<std::endl;
      }
      m_appointmentResult = appointment.ATField;
    }
}


void
RoutingProtocol::Clear()
{
  NS_LOG_FUNCTION_NOARGS();
  m_table.clear();
}

void
RoutingProtocol::AddEntry (const Ipv4Address &dest,
                           const Ipv4Address &mask,
                           const Ipv4Address &next,
                           uint32_t interface)
{
  NS_LOG_FUNCTION(this << dest << next << interface << mask << m_mainAddress);
  RoutingTableEntry RTE;
  RTE.destAddr = dest;
  RTE.mask = mask;
  RTE.nextHop = next;
  RTE.interface = interface;
  m_table[dest] = RTE;
}

void
RoutingProtocol::AddEntry (const Ipv4Address &dest,
                           const Ipv4Address &mask,
                           const Ipv4Address &next,
                           const Ipv4Address &interfaceAddress)
{
  NS_LOG_FUNCTION(this << dest << next << interfaceAddress << mask << m_mainAddress);

  NS_ASSERT (m_ipv4);

  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces(); ++i)
   for (uint32_t j = 0; j< m_ipv4->GetNAddresses(i); ++j)
     {
       if (m_ipv4->GetAddress(i,j).GetLocal() == interfaceAddress)
         {
           AddEntry(dest, mask, next, i);
           return;
         }
     }
  //ERROR NO MATCHING INTERFACES
  NS_ASSERT(false);
}

bool
RoutingProtocol::Lookup(Ipv4Address const &dest,
                        RoutingTableEntry &outEntry) const
{
  std::map<Ipv4Address, RoutingTableEntry>::const_iterator it =
    m_table.find(dest);
  if (it != m_table.end())
    {
      outEntry = it->second;
      return true;
    }
  else
    {
      Ipv4Mask MaskTemp;
      uint16_t max_prefix;
      bool max_prefix_meaningful = false;
      for (it = m_table.begin();it!=m_table.end(); ++it)
        {
          MaskTemp.Set (it->second.mask.Get ());
          if (MaskTemp.IsMatch (dest, it->second.destAddr))
            {
              if (!max_prefix_meaningful)
                {
                  max_prefix_meaningful = true;
                  max_prefix = MaskTemp.GetPrefixLength ();
                  outEntry = it->second;
                }
              if (max_prefix_meaningful && (max_prefix < MaskTemp.GetPrefixLength ()))
                {
                  max_prefix = MaskTemp.GetPrefixLength ();
                  outEntry = it->second;
                }
            }
        }
      if (max_prefix_meaningful)
        return true;
      else
        return false;
    }

}

void
RoutingProtocol::RemoveEntry (Ipv4Address const &dest)
{
  m_table.erase (dest);
}


bool
RoutingProtocol::RouteInput(Ptr<const Packet> p,
                            const Ipv4Header &header,
                            Ptr<const NetDevice> idev,
                            UnicastForwardCallback ucb,
                            MulticastForwardCallback mcb,
                            LocalDeliverCallback lcb,
                            ErrorCallback ecb)
{
  NS_LOG_FUNCTION (this << " " << m_ipv4->GetObject<Node> ()->GetId () << " " << header.GetDestination ());
  //TODO
  //std::cout<<m_mainAddress.Get ()%256<<" "<<header.GetDestination ().Get () %256<<std::endl;
  //bool lcb_status = false;
  Ipv4Address dest = header.GetDestination();
  Ipv4Address sour = header.GetSource();

  // Consume self-originated packets
  if (IsMyOwnAddress (sour) == true)
    {
      return true;
    }


  // Local delivery
  NS_ASSERT (m_ipv4->GetInterfaceForDevice (idev) >= 0);
  uint32_t iif = m_ipv4->GetInterfaceForDevice (idev);
  if (m_ipv4->IsDestinationAddress (dest, iif))
    {
      //Local delivery
      if (!lcb.IsNull ())
        {
          NS_LOG_LOGIC ("Broadcast local delivery to " << dest);
          lcb (p, header, iif);
          /*if ((m_mainAddress.Get ()%256 == 53)&&(iif=m_SCHinterface))
            {
              std::cout<<m_mainAddress.Get ()%256<<" "<<header.GetDestination ().Get () %256<<std::endl;
              std::cout<<"YES!"<<std::endl;
            }*/
        }
      else
        {
          NS_LOG_ERROR ("Unable to deliver packet locally due to null callback");
          ecb (p, header, Socket::ERROR_NOROUTETOHOST);
          return false;
        }

      //Broadcast forward
      if ((iif == m_SCHinterface) && (m_nodetype == CAR) && (m_appointmentResult == FORWARDER) && (sour != m_next_forwarder))
        {
          NS_LOG_LOGIC ("Forward broadcast");
          Ptr<Ipv4Route> broadcastRoute = Create<Ipv4Route> ();
          broadcastRoute->SetGateway (dest);//broadcast
          broadcastRoute->SetOutputDevice (m_ipv4->GetNetDevice (m_SCHinterface));
          Ipv4Header ipHeader = header;
          ipHeader.SetSource (m_mainAddress); //To Prevent Brocast Storm
          ucb (broadcastRoute, p, ipHeader);
        }
      return true;

    }
  //Drop
  return true;
}

void
RoutingProtocol::NotifyInterfaceUp (uint32_t i)
{}
void
RoutingProtocol::NotifyInterfaceDown (uint32_t i)
{}
void
RoutingProtocol::NotifyAddAddress (uint32_t interface, Ipv4InterfaceAddress address)
{}
void
RoutingProtocol::NotifyRemoveAddress (uint32_t interface, Ipv4InterfaceAddress address)
{}

Ptr<Ipv4Route>
RoutingProtocol::RouteOutput (Ptr<Packet> p,
             const Ipv4Header &header,
             Ptr<NetDevice> oif,
             Socket::SocketErrno &sockerr)
{
  NS_LOG_FUNCTION (this << " " << m_ipv4->GetObject<Node> ()->GetId () << " " << header.GetDestination () << " " << oif);
  Ptr<Ipv4Route> rtentry;
  RoutingTableEntry entry;
  //std::cout<<"RouteOutput "<<m_mainAddress.Get ()%256 << ",Dest:"<<header.GetDestination ().Get ()%256<<std::endl;
  //std::cout<<"M_TABLE SIZE "<<m_table.size ()<<std::endl;
  if (Lookup (header.GetDestination (), entry))
    {
      //std::cout<<"found!"<<std::endl;
      uint32_t interfaceIdx = entry.interface;
      if (oif && m_ipv4->GetInterfaceForDevice (oif) != static_cast<int> (interfaceIdx))
        {
          // We do not attempt to perform a constrained routing searchTx_Data_Pkts
          // if the caller specifies the oif; we just enforce that
          // that the found route matches the requested outbound interface
          NS_LOG_DEBUG ("SDN node " << m_mainAddress
                                     << ": RouteOutput for dest=" << header.GetDestination ()
                                     << " Route interface " << interfaceIdx
                                     << " does not match requested output interface "
                                     << m_ipv4->GetInterfaceForDevice (oif));
          sockerr = Socket::ERROR_NOROUTETOHOST;
          std::cout<<"does not match requested output interface"<<std::endl;
          return rtentry;
        }
      rtentry = Create<Ipv4Route> ();
      rtentry->SetDestination (header.GetDestination ());
      // the source address is the interface address that matches
      // the destination address (when multiple are present on the
      // outgoing interface, one is selected via scoping rules)
      NS_ASSERT (m_ipv4);
      uint32_t numOifAddresses = m_ipv4->GetNAddresses (interfaceIdx);
      NS_ASSERT (numOifAddresses > 0);
      Ipv4InterfaceAddress ifAddr;
      if (numOifAddresses == 1) {
          ifAddr = m_ipv4->GetAddress (interfaceIdx, 0);
        } else {
          NS_FATAL_ERROR ("XXX Not implemented yet:  IP aliasing and SDN");
        }
      rtentry->SetSource (ifAddr.GetLocal ());
      rtentry->SetGateway (entry.nextHop);
      rtentry->SetOutputDevice (m_ipv4->GetNetDevice (interfaceIdx));
      sockerr = Socket::ERROR_NOTERROR;
      NS_LOG_DEBUG ("SDN node " << m_mainAddress
                                 << ": RouteOutput for dest=" << header.GetDestination ()
                                 << " --> nextHop=" << entry.nextHop
                                 << " interface=" << entry.interface);
      NS_LOG_DEBUG ("Found route to " << rtentry->GetDestination () << " via nh " << rtentry->GetGateway () << " with source addr " << rtentry->GetSource () << " and output dev " << rtentry->GetOutputDevice ());
    }
  else
    {
      NS_LOG_DEBUG ("SDN node " << m_mainAddress
                                 << ": RouteOutput for dest=" << header.GetDestination ()
                                 << " No route to host");
      sockerr = Socket::ERROR_NOROUTETOHOST;
      std::cout<<"No route to host"<<std::endl;
    }
  return rtentry;
}

void
RoutingProtocol::Dump ()
{
#ifdef NS3_LOG_ENABLE
  NS_LOG_DEBUG ("Dumpping For" << m_mainAddress);
#endif //NS3_LOG_ENABLE
}

std::vector<RoutingTableEntry>
RoutingProtocol::GetRoutingTableEntries () const
{
  std::vector<RoutingTableEntry> rtvt;
  for (std::map<Ipv4Address, RoutingTableEntry>::const_iterator it = m_table.begin ();
       it != m_table.end (); ++it)
    {
      rtvt.push_back (it->second);
    }
  return rtvt;
}

int64_t
RoutingProtocol::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_uniformRandomVariable->SetStream (stream);
  return 1;
}

uint16_t
RoutingProtocol::GetPacketSequenceNumber ()
{
  m_packetSequenceNumber = (m_packetSequenceNumber + 1) % (SDN_MAX_SEQ_NUM + 1);
  return m_packetSequenceNumber;
}


uint16_t
RoutingProtocol::GetMessageSequenceNumber ()
{
  m_messageSequenceNumber = (m_messageSequenceNumber + 1) % (SDN_MAX_SEQ_NUM + 1);
  return m_messageSequenceNumber;
}

void
RoutingProtocol::HelloTimerExpire ()
{
  if (GetType() == CAR)
    {
      SendHello ();
      m_helloTimer.Schedule (m_helloInterval);
    }
}

void
RoutingProtocol::RmTimerExpire ()
{
  //Do nothing.
}

void
RoutingProtocol::APTimerExpire ()
{
  if (GetType() == LOCAL_CONTROLLER)
    {
      ComputeRoute ();
    }
}


// SDN packets actually send here.
void
RoutingProtocol::SendPacket (Ptr<Packet> packet,
                             const MessageList &containedMessages)
{
  NS_LOG_DEBUG ("SDN node " << m_mainAddress << " sending a SDN packet");
  // Add a header
  sdn::PacketHeader header;
  header.originator = this->m_mainAddress;
  header.SetPacketLength (header.GetSerializedSize () + packet->GetSize ());
  header.SetPacketSequenceNumber (GetPacketSequenceNumber ());
  packet->AddHeader (header);

  // Trace it
  m_txPacketTrace (header, containedMessages);

  // Send it
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator i =
         m_socketAddresses.begin (); i != m_socketAddresses.end (); ++i)
    {
      Ipv4Address bcast = i->second.GetLocal ().GetSubnetDirectedBroadcast (i->second.GetMask ());
      i->first->SendTo (packet, 0, InetSocketAddress (bcast, SDN_PORT_NUMBER));
    }
}

void
RoutingProtocol::QueueMessage (const sdn::MessageHeader &message, Time delay)
{
   m_queuedMessages.push_back (message);
  if (not m_queuedMessagesTimer.IsRunning ())
    {
      m_queuedMessagesTimer.SetDelay (delay);
      m_queuedMessagesTimer.Schedule ();
    }
}


// NS3 is not multithread, so mutex is unnecessary.
// Here, messages will queue up and send once numMessage is equl to SDN_MAX_MSGS.
// This function will NOT add a header to each message
void
RoutingProtocol::SendQueuedMessages ()
{
  Ptr<Packet> packet = Create<Packet> ();
  int numMessages = 0;

  NS_LOG_DEBUG ("SDN node " << m_mainAddress << ": SendQueuedMessages");
  //std::cout<<"SendQueuedMessages  "<<m_mainAddress.Get ()%256 <<std::endl;
  MessageList msglist;

  for (std::vector<sdn::MessageHeader>::const_iterator message = m_queuedMessages.begin ();
       message != m_queuedMessages.end ();
       ++message)
    {
      Ptr<Packet> p = Create<Packet> ();
      p->AddHeader (*message);
      packet->AddAtEnd (p);
      msglist.push_back (*message);
      if (++numMessages == SDN_MAX_MSGS)
        {
          SendPacket (packet, msglist);
          msglist.clear ();
          // Reset variables for next packet
          numMessages = 0;
          packet = Create<Packet> ();
        }
    }

  if (packet->GetSize ())
    {
      SendPacket (packet, msglist);
    }

  m_queuedMessages.clear ();
}

bool
RoutingProtocol::IsMyOwnAddress (const Ipv4Address & a) const
{
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator j =
         m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ipv4InterfaceAddress iface = j->second;
      if (a == iface.GetLocal ())
        {
          return true;
        }
    }
  return false;
}

void
RoutingProtocol::SendHello ()
{
  NS_LOG_FUNCTION (this);
  sdn::MessageHeader msg;
  Time now = Simulator::Now ();
  msg.SetVTime (m_helloInterval);
  msg.SetTimeToLive (41993);//Just MY Birthday.
  msg.SetMessageSequenceNumber (GetMessageSequenceNumber ());
  msg.SetMessageType (sdn::MessageHeader::HELLO_MESSAGE);

  sdn::MessageHeader::Hello &hello = msg.GetHello ();
  hello.ID = m_mainAddress;
  Vector pos = m_mobility->GetPosition ();
  Vector vel = m_mobility->GetVelocity ();
  hello.SetPosition (pos.x, pos.y, pos.z);
  hello.SetVelocity (vel.x, vel.y, vel.z);

  NS_LOG_DEBUG ( "SDN HELLO_MESSAGE sent by node: " << hello.ID
                 << "   at " << now.GetSeconds() << "s");
  QueueMessage (msg, JITTER);
}

void
RoutingProtocol::SendRoutingMessage ()
{
  NS_LOG_FUNCTION (this);

  for (std::map<Ipv4Address, CarInfo>::const_iterator cit = m_lc_info.begin ();
       cit != m_lc_info.end (); ++cit)
    {
      sdn::MessageHeader msg;
      Time now = Simulator::Now ();
      msg.SetVTime (m_helloInterval);
      msg.SetTimeToLive (41993);//Just MY Birthday.
      msg.SetMessageSequenceNumber (GetMessageSequenceNumber ());
      msg.SetMessageType (sdn::MessageHeader::ROUTING_MESSAGE);
      sdn::MessageHeader::Rm &rm = msg.GetRm ();
      rm.ID = cit->first;
      sdn::MessageHeader::Rm::Routing_Tuple rt;
      for (std::vector<RoutingTableEntry>::const_iterator cit2 = cit->second.R_Table.begin ();
           cit2 != cit->second.R_Table.end (); ++cit2)
        {
          rt.destAddress = cit2->destAddr;
          rt.mask = cit2->mask;
          rt.nextHop = cit2->nextHop;
          rm.routingTables.push_back (rt);
        }
      rm.routingMessageSize = rm.routingTables.size ();
      QueueMessage (msg, JITTER);
    }
}

void
RoutingProtocol::SendAppointment ()
{
  NS_LOG_FUNCTION (this);

  for (std::map<Ipv4Address, CarInfo>::const_iterator cit = m_lc_info.begin ();
       cit != m_lc_info.end (); ++cit)
    {
      sdn::MessageHeader msg;
      Time now = Simulator::Now ();
      msg.SetVTime (m_helloInterval);
      msg.SetTimeToLive (41993);//Just MY Birthday.
      msg.SetMessageSequenceNumber (GetMessageSequenceNumber ());
      msg.SetMessageType (sdn::MessageHeader::APPOINTMENT_MESSAGE);
      sdn::MessageHeader::Appointment &appointment = msg.GetAppointment ();
      appointment.ID = cit->first;
      appointment.ATField = cit->second.appointmentResult;
      appointment.NextForwarder = cit->second.ID_of_minhop;
      QueueMessage (msg, JITTER);
    }
}

void
RoutingProtocol::SetMobility (Ptr<MobilityModel> mobility)
{
  m_mobility = mobility;
}

void
RoutingProtocol::SetType (NodeType nt)
{
  m_nodetype = nt;
}

NodeType
RoutingProtocol::GetType () const
{
  return m_nodetype;
}

void
RoutingProtocol::ComputeRoute ()
{
  std::cout<<"RemoveTimeOut"<<std::endl;
  RemoveTimeOut (); //Remove Stale Tuple

  if (1)//(!m_linkEstablished)
    {
      std::cout<<"Do_Init_Compute"<<std::endl;
      Do_Init_Compute ();
    }
  else
    {
      std::cout<<"Do_Update"<<std::endl;
      Do_Update ();
    }

  std::cout<<"SendAppointment"<<std::endl;
  SendAppointment ();
  std::cout<<"Reschedule"<<std::endl;
  Reschedule ();
  std::cout<<"CR DONE"<<std::endl;
}//RoutingProtocol::ComputeRoute

void
RoutingProtocol::Do_Init_Compute ()
{
  std::cout<<"Partition"<<std::endl;
  Partition ();

  std::cout<<"SetN_Init"<<std::endl;
  SetN_Init ();

  std::cout<<"OtherSet_Init"<<std::endl;
  OtherSet_Init ();

  std::cout<<"Next:";
  for (std::map<Ipv4Address, CarInfo>::const_iterator cit=m_lc_info.begin ();
       cit!=m_lc_info.end (); ++cit)
    {
      std::cout<<cit->first.Get ()%256<<"->"<<cit->second.ID_of_minhop.Get ()%256<<","<<cit->second.minhop<<";";
    }
  std::cout<<std::endl;

  std::cout<<"SelectNode"<<std::endl;
  SelectNode ();
  std::cout<<"Do_Init_Compute DONE"<<std::endl;
}

void
RoutingProtocol::Do_Update ()
{
  std::cout<<"Partition"<<std::endl;
  Partition ();
  std::cout<<"CalcSetZero"<<std::endl;
  CalcSetZero ();
  std::cout<<"SelectNewNodeInAreaZero"<<std::endl;
  SelectNewNodeInAreaZero ();
  std::cout<<"Do_Update DONE"<<std::endl;
}

void
RoutingProtocol::Partition ()
{
  m_Sections.clear ();
  int numArea = GetNumArea();
  for (int i = 0; i < numArea; ++i)
    {
      m_Sections.push_back (std::set<Ipv4Address> ());
    }
  for (std::map<Ipv4Address, CarInfo>::const_iterator cit = m_lc_info.begin ();
       cit != m_lc_info.end(); ++cit)
    {
      m_Sections[GetArea (cit->second.GetPos ())].insert (cit->first);
    }
  std::cout<<m_lc_info.size ()<<std::endl;
  for (int i = 0; i < numArea; ++i)
    {
      std::cout<<"Section "<<i<<": ";
      for (std::set<Ipv4Address>::const_iterator cit = m_Sections[i].begin ();
           cit != m_Sections[i].end (); ++cit)
        {
          std::cout<<cit->Get ()%256<<",";
        }
      std::cout<<std::endl;
    }
}

void
RoutingProtocol::SetN_Init ()
{
  int numArea = GetNumArea();
  for (std::set<Ipv4Address>::const_iterator cit = m_Sections[numArea-1].begin ();
       cit != m_Sections[numArea-1].end (); ++cit)
    {
      m_lc_info[(*cit)].minhop = 1;
      m_lc_info[(*cit)].ID_of_minhop = Ipv4Address::GetZero ();
    }
}

void
RoutingProtocol::OtherSet_Init ()
{
  int numArea = GetNumArea();
  //m_lc_info.clear (); WTF?
  for (int area = numArea - 2; area >= 0; --area)
    {
      for (std::set<Ipv4Address>::const_iterator cit = m_Sections[area].begin ();
           cit != m_Sections[area].end (); ++cit)
        {
          m_lc_info[(*cit)].minhop = INFHOP;
          m_lc_info[(*cit)].ID_of_minhop = Ipv4Address::GetZero ();
        }
      m_lc_shorthop.clear();
      SortByDistance (area);
      CalcShortHopOfArea (area, area + 1);
      if ((area == numArea - 3) && isPaddingExist ())
        {
          CalcShortHopOfArea (area, area + 2);
        }
      CalcIntraArea (area);
    }
}

void
RoutingProtocol::SortByDistance (int area)
{
  m_list4sort.clear ();
  for (std::set<Ipv4Address>::const_iterator cit = m_Sections[area].begin ();
      cit != m_Sections[area].end (); ++cit)
    {
      bool done = false;
      for (std::list<Ipv4Address>::iterator it = m_list4sort.begin ();
           it != m_list4sort.end (); ++it)
        {
          if (m_lc_info[*it].GetPos ().x < m_lc_info[*cit].GetPos ().x)
            {
              m_list4sort.insert (it, *cit);
              done = true;
              break;
            }
        }
      if (!done)
        {
          m_list4sort.push_back (*cit);
        }
    }
}

void
RoutingProtocol::CalcShortHopOfArea (int fromArea, int toArea)
{
  for (std::list<Ipv4Address>::const_iterator cit = m_list4sort.begin ();
       cit != m_list4sort.end (); ++cit)
    {
      for (std::set<Ipv4Address>::const_iterator cit2 = m_Sections[toArea].begin ();
           cit2 != m_Sections[toArea].end (); ++cit2)
        {
          m_lc_shorthop[*cit].push_back (GetShortHop (*cit,*cit2));
        }

      UpdateMinHop (*cit);
    }
}

void
RoutingProtocol::UpdateMinHop (const Ipv4Address &ID)
{
  uint32_t theminhop = INFHOP;
  Ipv4Address IDofminhop = Ipv4Address::GetZero ();
  for (std::list<ShortHop>::const_iterator cit = m_lc_shorthop[ID].begin ();
       cit != m_lc_shorthop[ID].end (); ++cit)
    {
      if (cit->hopnumber < theminhop)
        {
          theminhop = cit->hopnumber;
          if (cit->isTransfer)
            {
              IDofminhop = cit->proxyID;
            }
          else
            {
              IDofminhop = cit->nextID;
            }
        }
    }
  if (theminhop < m_lc_info[ID].minhop)
    {
      m_lc_info[ID].ID_of_minhop = IDofminhop;
      m_lc_info[ID].minhop = theminhop;
    }
}

void
RoutingProtocol::CalcIntraArea (int area)
{
  CalcShortHopOfArea (area, area);
}

void
RoutingProtocol::SelectNode ()
{
  //4-1
  ResetAppointmentResult ();
  uint32_t thezero = 0;
  Ipv4Address The_Car(thezero);
  uint32_t minhop_of_tc = INFHOP;
  double best_pos = m_signal_range;

  //First Area
  for (std::set<Ipv4Address>::const_iterator cit = m_Sections[0].begin ();
      cit != m_Sections[0].end (); ++cit)
    {
      CarInfo& temp_info = m_lc_info[*cit];
      if (temp_info.minhop < minhop_of_tc)
        {
          minhop_of_tc = temp_info.minhop;
          best_pos = temp_info.Position.x;
          The_Car = *cit;
        }
      else
        if ((temp_info.minhop == minhop_of_tc)&&(temp_info.Position.x < best_pos)&&(minhop_of_tc < INFHOP))
          {
            best_pos = temp_info.Position.x;
            The_Car = *cit;
          }
    }
  m_theFirstCar = The_Car;
  Ipv4Address ZERO = Ipv4Address::GetZero ();
  std::cout<<"Chain ";
  if (The_Car != ZERO)
    {
      m_linkEstablished = true;
    }
  else
    {
      m_linkEstablished = false;
    }
  while (The_Car != ZERO)
    {
      double oldp = m_lc_info[The_Car].GetPos ().x;
      std::cout<<The_Car.Get () % 256<<"("<<oldp<<","<<m_lc_info[The_Car].minhop<<")";
      m_lc_info[The_Car].appointmentResult = FORWARDER;
      The_Car = m_lc_info[The_Car].ID_of_minhop;
      if (The_Car != ZERO)
        {
          std::cout<<"<-"<<m_lc_info[The_Car].GetPos ().x - oldp<<"->";
        }
    }
  std::cout<<std::endl;
}

void
RoutingProtocol::ResetAppointmentResult ()
{
  for (std::map<Ipv4Address, CarInfo>::iterator it = m_lc_info.begin ();
       it != m_lc_info.end (); ++it)
    {
      it->second.appointmentResult = NORMAL;
    }
}

void
RoutingProtocol::CalcSetZero ()
{
  m_lc_shorthop.clear();
  SortByDistance (0);
  if (GetNumArea () > 1)
    CalcShortHopOfArea (0,1);
  if ((GetNumArea () == 3)&&(isPaddingExist ()))
    CalcShortHopOfArea (0,2);
  CalcIntraArea (0);
}

void
RoutingProtocol::SelectNewNodeInAreaZero ()
{
  uint32_t thezero = 0;
  Ipv4Address The_Car (thezero);
  uint32_t minhop_of_tc = INFHOP;
  for (std::set<Ipv4Address>::const_iterator cit = m_Sections[0].begin ();
       cit != m_Sections[0].end (); ++cit)
    {
      CarInfo& temp_info = m_lc_info[*cit];
      if (temp_info.minhop < minhop_of_tc)
        {
          minhop_of_tc = temp_info.minhop;
          The_Car = *cit;
        }
      else
        if (temp_info.minhop == minhop_of_tc)
          {
            if (temp_info.ID_of_minhop == m_theFirstCar)
              {
                minhop_of_tc = temp_info.minhop;
                The_Car = *cit;
              }
          }
    }

  if (The_Car != Ipv4Address::GetZero ())
    {
      if (m_lc_info[The_Car].ID_of_minhop == m_theFirstCar)
        {
          m_theFirstCar = The_Car;
          m_lc_info[The_Car].appointmentResult = FORWARDER;
          std::cout<<The_Car.Get () % 256<<"YES"<<std::endl;
        }
      else
        {
          ResetAppointmentResult ();
          m_theFirstCar = The_Car;
          std::cout<<"Chain ";
          while (m_lc_info.find (The_Car) != m_lc_info.end ())
            {
              double oldp = m_lc_info[The_Car].GetPos ().x;
              std::cout<<The_Car.Get () % 256<<"("<<oldp<<","<<m_lc_info[The_Car].minhop<<")";
              m_lc_info[The_Car].appointmentResult = FORWARDER;
              The_Car = m_lc_info[The_Car].ID_of_minhop;
              if (The_Car != Ipv4Address::GetZero ())
                {
                  std::cout<<"<-"<<m_lc_info[The_Car].GetPos ().x - oldp<<"->";
                }
            }
          std::cout<<std::endl;
        }
    }
  else
    {
      m_linkEstablished = false;
    }
}

void
RoutingProtocol::Reschedule ()
{
  if (!m_linkEstablished)
    {
      if (m_apTimer.IsRunning ())
        {
          m_apTimer.Remove ();
        }
      m_apTimer.Schedule (m_minAPInterval);
      std::cout<<"Reschedule:"<<m_minAPInterval.GetSeconds ()<<"s."<<std::endl;
    }
  else
    {
      double vx = m_lc_info[m_theFirstCar].Velocity.x;
      double px = m_lc_info[m_theFirstCar].GetPos ().x;
      double t2l;
      if (vx < 1e-7)
        {
          t2l = m_minAPInterval.GetSeconds ();
        }
      else
        {
          t2l= (0.5 * m_signal_range - px) / vx;
        }
      if (m_apTimer.IsRunning ())
        {
          m_apTimer.Remove ();
        }
      if (t2l<1e-5)
        {
          t2l = m_minAPInterval.GetSeconds ();
        }
      m_apTimer.Schedule(Seconds(t2l));
      std::cout<<"Reschedule:"<<t2l<<"s."<<"p:"<<px<<",v:"<<vx<<std::endl;
    }
}

ShortHop
RoutingProtocol::GetShortHop(const Ipv4Address& IDa, const Ipv4Address& IDb)
{
  double const vxa = m_lc_info[IDa].Velocity.x,
               vxb = m_lc_info[IDb].Velocity.x;
  //Predict
  double const pxa = m_lc_info[IDa].GetPos ().x,
               pxb = m_lc_info[IDb].GetPos ().x;
  // time to b left
  double temp;
  if (vxb > 0)
    {
      temp = (m_road_length - pxb) / vxb;
    }
  else
    {
      //b is fixed.
      temp = (m_road_length - pxa) / vxa;
    }
  double const t2bl = temp;
  if ((pxb - pxa < m_signal_range) && (abs((pxb + vxb*t2bl)-(pxa + vxa*t2bl)) < m_signal_range))
    {
      ShortHop sh;
      sh.nextID = IDb;
      sh.hopnumber = m_lc_info[IDb].minhop + 1;
      sh.isTransfer = false;
      return sh;
    }//if ((pxb -  ...
  else
    {
      ShortHop sh;
      sh.isTransfer = true;
      sh.t = 0; // Time when connection loss
      sh.hopnumber = INFHOP;
      if (pxb - pxa < m_signal_range)
        {
          if (vxb > vxa)
            {
              sh.t = (m_signal_range + pxa - pxb) / (vxb - vxa);
            }
          else
            {
              sh.t = (m_signal_range + pxb - pxa) / (vxa - vxb);
            }
        }
      //Find another car
      for (std::map<Ipv4Address, CarInfo>::const_iterator cit = m_lc_info.begin ();
           cit != m_lc_info.end (); ++cit)
        {
          double const vxc = cit->second.Velocity.x;
          //pxc when t
          double const tpxc = cit->second.GetPos ().x + vxc * sh.t;
          //pxa and pxb when t
          double const tpxa = pxa + vxa * sh.t,
                       tpxb = pxb + vxb * sh.t;
          //t2bl minus t
          double const t2blmt = t2bl - sh.t;
          if ((tpxa<tpxc)&&(tpxc<tpxb)&&(tpxc-tpxa<m_signal_range)&&(tpxb-tpxc<m_signal_range))
            {
              if ((abs((tpxb + vxb*t2blmt)-(tpxc + vxc*t2blmt)) < m_signal_range)&&
                  (abs((tpxc + vxc*t2blmt)-(tpxa + vxa*t2blmt)) < m_signal_range))
                {
                  sh.IDa = IDa;
                  sh.IDb = IDb;
                  sh.proxyID = cit->first;
                  sh.hopnumber = m_lc_info[IDb].minhop + 2;
                  return sh;
                }//if ((abs((tpxb ...
            }//if ((tpxa ...
        }//for (std::map<I ...
      return sh;
    }//else
}

void
RoutingProtocol::LCAddEntry(const Ipv4Address& ID,
                            const Ipv4Address& dest,
                            const Ipv4Address& mask,
                            const Ipv4Address& next)
{
  CarInfo& Entry = m_lc_info[ID];
  RoutingTableEntry RTE;
  RTE.destAddr = dest;
  RTE.mask = mask;
  RTE.nextHop = next;
  Entry.R_Table.push_back (RTE);
}

void
RoutingProtocol::ClearAllTables ()
{
  for (std::map<Ipv4Address, CarInfo>::iterator it = m_lc_info.begin (); it!=m_lc_info.end(); ++it)
    {
      it->second.R_Table.clear ();
    }
}

int
RoutingProtocol::GetArea (Vector3D position) const
{
  //std::cout<<"P"<<position.x<<";";
  double &px = position.x;
  double road_length = m_road_length;
  //0.5r ~ r ~ r ~...~ r ~ r ~ last (if length_of_last<=0.5r, last={0.5r}; else last = {padding_area, 0.5r});
  if (px < 0.5*m_signal_range)
    {
      //std::cout<<"RET1"<<std::endl;
      return 0;
    }
  else
    {
      road_length -= 0.5*m_signal_range;
      int numOfTrivialArea = road_length / m_signal_range;
      double remain = road_length - (numOfTrivialArea * m_signal_range);
      if (!(remain>0))
        numOfTrivialArea--;

      px -= 0.5*m_signal_range;
      if (px < numOfTrivialArea * m_signal_range)
        {
          return (px / m_signal_range) + 1;
        }
      else
        {
          if (road_length - px < 0.5*m_signal_range)
            {
              if (isPaddingExist())
                return numOfTrivialArea + 2;
              else
                return numOfTrivialArea + 1;
            }
          else
            {
              return numOfTrivialArea + 1;
            }
        }

    }

}

int
RoutingProtocol::GetNumArea () const
{
  return m_numArea;
}

void
RoutingProtocol::Init_NumArea ()
{
  int ret;
  double road_length = m_road_length;
  if (road_length < 0.5*m_signal_range)
    {
      ret = 1;
    }
  else
    {
      road_length -= 0.5*m_signal_range;
      int numOfTrivialArea = road_length / m_signal_range;
      double last_length = road_length - (m_signal_range * numOfTrivialArea);
      if (last_length < 1e-10)//last_length == 0 Devied the last TrivialArea into 2
        {
          ret = 1 + (numOfTrivialArea - 1) + 1 + 1;//First Area + TrivialArea-1 + Padding + LastArea;
          m_isPadding = true;
        }
      else
        if (last_length > 0.5*m_signal_range)//0.5r<last_length<r
          {
            ret = 1 + numOfTrivialArea + 2;//First Area + TrivialArea + paddingArea +LastArea;
            m_isPadding = true;
          }
        else//0<last_length<0.5r
          {
            ret = 1 + numOfTrivialArea + 1;//First Area + TrivialArea + LastArea;
            m_isPadding = false;
          }
    }
  m_numArea = ret;
  m_numAreaVaild = true;
}

bool
RoutingProtocol::isPaddingExist () const
{
  return m_isPadding;
}

void
RoutingProtocol::RemoveTimeOut()
{
  Time now = Simulator::Now ();
  std::map<Ipv4Address, CarInfo>::iterator it = m_lc_info.begin ();
  std::vector<Ipv4Address> pendding;
  while (it != m_lc_info.end ())
    {
      if (now.GetSeconds() - it->second.LastActive.GetSeconds () > 3 * m_helloInterval.GetSeconds())
        {
          pendding.push_back (it->first);
        }
      ++it;
    }
  for (std::vector<Ipv4Address>::iterator it = pendding.begin ();
      it != pendding.end(); ++it)
    {
      m_lc_info.erase((*it));
    }
}

void
RoutingProtocol::SetSignalRangeNRoadLength (double signal_range, double road_length)
{
  m_signal_range = signal_range;
  m_road_length = road_length;
}

} // namespace sdn
} // namespace ns3


