#ifndef MOCKTURTLE_CONVERTER_H
#define MOCKTURTLE_CONVERTER_H

#include "include/core/Netlist.h"
#include <mockturtle/networks/xag.hpp>
#include <mockturtle/networks/aig.hpp>


// 將自定義的 Netlist 轉換為 Mockturtle 的 XAG 網路格式
mockturtle::xag_network NetlistToXag(const Netlist& nl);

// 將自定義的 Netlist 轉換為 Mockturtle 的 AIG 網路格式
mockturtle::aig_network NetlistToAig(const Netlist& nl);

// 將優化後的 XAG 網路轉換為全新的 Netlist
Netlist XagToNetlist(const mockturtle::xag_network& xag, const Netlist& old_nl);

// 將優化後的 AIG 網路轉換為全新的 Netlist
Netlist AigToNetlist(const mockturtle::aig_network& aig, const Netlist& old_nl);

// 合併輸入同一條 net 的重複 NOT：保留第一顆，其餘 fanout 改接、標死。
int mergeDuplicateInverters(Netlist& netlist);

// 把所有「吃 fromNet 當輸入」的閘，改成吃 toNet；並搬移 loadGateIds。
void redirectNetLoads(Netlist& netlist, int fromNet, int toNet);

    // 把一顆閘標死並從其 fanin 的 loadGateIds 斷開。
void detachGate(Netlist& netlist, int gid);

#endif // MOCKTURTLE_CONVERTER_H