
#include <cmath>
#include <cstring>

#include "flow-encoder.h"
#include "openflow-switch-net-device.h"
#include "flow-hash.h"

#include "ns3/log.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/tcp-l4-protocol.h"


namespace ns3 {
  
NS_LOG_COMPONENT_DEFINE("FlowEncoder");

NS_OBJECT_ENSURE_REGISTERED(FlowEncoder);

  
FlowEncoder::FlowEncoder()
{
  Clear();
}

FlowEncoder::~FlowEncoder()
{
}

void
FlowEncoder::SetOFSwtch (Ptr<NetDevice> OFswtch, int id)
{
  NS_LOG_FUNCTION(this);

  m_id = id;
  
  OFswtch->SetPromiscReceiveCallback(MakeCallback(&FlowEncoder::ReceiveFromOpenFlowSwtch, this));

}

int
FlowEncoder::GetID()
{
  return m_id;
}

FlowEncoder::CountTable_t&
FlowEncoder::GetCountTable()
{
  return m_countTable;
}

bool
FlowEncoder::ContainsFlow (const FlowField& flow)
{
  std::vector<uint32_t> filterIdxs = GetFlowFilterIdx(flow);

  bool hasFlow = true;

  for(unsigned ith = 0; ith < NUM_FLOW_HASH; ++ith)
    {
      if( !m_flowFilter[filterIdxs[ith]] )
	{
	  return false;
	}
    }
  
  return hasFlow;
}

void
FlowEncoder::ClearFlowInCountTable(const FlowField& flow)
{
  std::vector<uint32_t> tableIdxs = GetCountTableIdx (flow);
  for(unsigned ith = 0; ith < NUM_COUNT_HASH; ++ith )
    {     
      CountTableEntry& entry = m_countTable[tableIdxs[ith]];
      entry.XORFlow (flow);
      entry.flow_cnt   --;
      NS_ASSERT (entry.flow_cnt >= 0);
    }
}
  
bool
FlowEncoder::ReceiveFromOpenFlowSwtch(Ptr<NetDevice> ofswtch,
				      Ptr<const Packet> constPacket,
				      uint16_t protocol,
				      const Address& src, const Address& dst,
				      NetDevice::PacketType packetType)
{
  NS_LOG_INFO("FlowEncoder ID " <<m_id);
  
  Ptr<Packet> packet    = constPacket->Copy();

  FlowField   flow      = FlowFieldFromPacket (packet, protocol);
  NS_LOG_INFO(flow);
  bool        isNewFlow = UpdateFlowFilter (flow);   
  if (isNewFlow) NS_LOG_INFO("New flow");
  UpdateCountTable (flow, isNewFlow);
  
  return true;
}

void
FlowEncoder::Clear()
{
  NS_LOG_INFO("FlowEncoder ID " <<m_id << " reset");
  m_flowFilter.reset();
  m_countTable.clear();
  m_countTable.resize(COUNT_TABLE_SIZE, CountTableEntry());
}

bool
FlowEncoder::UpdateFlowFilter(const FlowField& flow)
{
  std::vector<uint32_t> filterIdxs = GetFlowFilterIdx(flow);

  bool isNew = false;

  for(unsigned ith = 0; ith < NUM_FLOW_HASH; ++ith)
    {
      if( !m_flowFilter[filterIdxs[ith]] )
	{
	  //new flow
	  m_flowFilter[filterIdxs[ith]] = true;
	  isNew = true;
	}
    }
  
  return isNew;
}

void
FlowEncoder::UpdateCountTable(const FlowField& flow, bool isNew)
{

  std::vector<uint32_t> tableIdxs = GetCountTableIdx(flow); 
  //if is new, update the flow fields.
  if (isNew)
    {
      for(unsigned ith = 0; ith < NUM_COUNT_HASH; ++ith)
	{
	  CountTableEntry& entry = m_countTable[tableIdxs[ith]];

	  NS_ASSERT (entry.flow_cnt < 256);
	  
	  entry.XORFlow(flow);
	  entry.flow_cnt += 1;
	}
    }
  
  //update packet count
  for(unsigned ith = 0; ith < NUM_COUNT_HASH; ++ith)
    {
      m_countTable[tableIdxs[ith]].packet_cnt++; 
    }

}


std::vector<uint32_t>
FlowEncoder::GetFlowFilterIdx(const FlowField& flow)
{
  std::vector<uint32_t> filterIdxs;
  
  char buf[13];
  for(unsigned ith = 0; ith < NUM_FLOW_HASH; ++ith)
    {

      memset(buf, 0, 13);
      memcpy(buf     , &(flow.ipv4srcip), 4);
      memcpy(buf + 4 , &(flow.ipv4dstip), 4);
      memcpy(buf + 8 , &(flow.srcport)  , 2);
      memcpy(buf + 10, &(flow.dstport)  , 2);
      memcpy(buf + 12, &(flow.ipv4prot) , 1);
      
      //ith is also work as a seed of hash function
      uint32_t idx = murmur3_32(buf, 13, ith);

      //according to the P4 modify_field_with_hash_based_offset
      //the idx value is generated by %size;
      idx %= FLOW_FILTER_SIZE;
      filterIdxs.push_back(idx);
    }

  return filterIdxs;
}

std::vector<uint32_t>
FlowEncoder::GetCountTableIdx(const FlowField& flow)
{
  std::vector<uint32_t> tableIdxs;
  
  char buf[13];
  for(unsigned ith = 0; ith < NUM_COUNT_HASH; ++ith)
    {

      memset(buf, 0, 13);
      memcpy(buf     , &(flow.ipv4srcip), 4);
      memcpy(buf + 4 , &(flow.ipv4dstip), 4);
      memcpy(buf + 8 , &(flow.srcport)  , 2);
      memcpy(buf + 10, &(flow.dstport)  , 2);
      memcpy(buf + 12, &(flow.ipv4prot) , 1);
      
      //ith is also work as a seed of hash function
      uint32_t idx    = murmur3_32(buf, 13, ith);
      uint32_t offset = ith * COUNT_TABLE_SUB_SIZE;
      
      //according to the P4 modify_field_with_hash_based_offset
      //the idx value is generated by %size;
      idx = offset + (idx % COUNT_TABLE_SUB_SIZE);
      tableIdxs.push_back(idx);
    }
  return tableIdxs;
}


FlowField
FlowEncoder::FlowFieldFromPacket(Ptr<Packet> packet, uint16_t protocol) const
{
  NS_LOG_INFO("Extract flow field");
    
  FlowField flow;
  if(protocol == Ipv4L3Protocol::PROT_NUMBER)
    {
      Ipv4Header ipHd;
      if( packet->PeekHeader(ipHd) )
	{
	  //NS_LOG_INFO("IP header detected");
	  
	  flow.ipv4srcip = ipHd.GetSource().Get();
	  flow.ipv4dstip = ipHd.GetDestination().Get();
	  flow.ipv4prot  = ipHd.GetProtocol ();
	  packet->RemoveHeader (ipHd);

	  if( flow.ipv4prot == TcpL4Protocol::PROT_NUMBER)
	    {
	      TcpHeader tcpHd;
	      if( packet->PeekHeader(tcpHd) )
		{
		  //NS_LOG_INFO("TCP header detected");
		  
		  flow.srcport = tcpHd.GetSourcePort ();
		  flow.dstport = tcpHd.GetDestinationPort ();
		  packet->RemoveHeader(tcpHd);

		}
	    }
	  else if( flow.ipv4prot == UdpL4Protocol::PROT_NUMBER )
	    {
	      UdpHeader udpHd;
	      if( packet->PeekHeader(udpHd))
		{
		  //NS_LOG_INFO("UDP header detected");
		 
		  flow.srcport = udpHd.GetSourcePort ();
		  flow.dstport = udpHd.GetDestinationPort ();
		  packet->RemoveHeader(udpHd);
		}
	    }
	  else
	    {
	      NS_LOG_INFO("layer 4 protocol can't extract: "<< unsigned(flow.ipv4prot));
	    }
	  
	}
    }
  else
    {
      NS_LOG_INFO("packet is not an ip packet");
    }

  return flow;
}


}

