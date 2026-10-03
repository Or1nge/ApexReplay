#pragma once
#include <algorithm>
#include <cstdint>
namespace apex {
inline uint64_t replayMemoryBudget(uint64_t available,uint64_t total,uint64_t encoded,double percent){
    // Restore our own encoded allocation to the available-memory measurement to
    // avoid shrinking the allowance merely because this replay cache grew.
    auto externalIdle=std::min(total,available+std::min(encoded,total-std::min(total,available)));
    uint64_t reserve=std::min<uint64_t>(2ull<<30,total/10);
    return std::min<uint64_t>(static_cast<uint64_t>(externalIdle*std::clamp(percent,10.0,90.0)/100),total-reserve);
}
}
