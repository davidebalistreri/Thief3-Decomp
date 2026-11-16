#pragma once

#include "scene_bridge_visible_lights.hpp"
#include "../ir/scene_live.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace t3render::live {
static_assert(sizeof(float)==sizeof(std::uint32_t), "light samples require 32-bit floats");

// Converts only the candidate position. The other raw words have no proven
// physical or color-space interpretation and never enter SceneIR.
inline bool BuildVisibleLightCandidates(const scene_bridge::VisibleLightCandidates& source,
                                        const std::string& map,const std::string& map_sha256,
                                        std::uint64_t generation,std::vector<VisualLight>& output) {
    if (!source.stable || source.count>scene_bridge::kMaxVisibleLightCandidates || !generation ||
        map.empty() || map.size()>128 || map_sha256.size()!=64) return false;
    std::vector<VisualLight> converted;converted.reserve(source.count);
    for (std::uint32_t i=0;i<source.count;++i) {
        const auto& item=source.items[i];
        const auto* end=static_cast<const char*>(std::memchr(item.actor_name,'\0',sizeof(item.actor_name)));
        if (!end || end==item.actor_name) return false;
        // Keep the base and FName number separate: a literal base ending in
        // "__15" must not collide with a numbered instance of "Light".
        std::string name(item.actor_name,end);
        if (item.actor_number>0xffff || name.find(':')!=std::string::npos) return false;
        VisualLight light;light.id="light-actor:"+map+":"+map_sha256+":"+name+":"+std::to_string(item.actor_number);
        light.generation=generation;light.binding=BindingEvidence::Candidate;
        Vec3 position{};bool valid=true;
        for (unsigned axis=0;axis<3;++axis) {
            std::memcpy(&position[axis],&item.words.position_34[axis],sizeof(float));
            valid=valid && std::isfinite(position[axis]) && std::fabs(position[axis])<=1000000;
        }
        if (valid) light.position={position,BindingEvidence::Candidate};
        converted.push_back(std::move(light));
    }
    output=std::move(converted);return true;
}
} // namespace t3render::live
