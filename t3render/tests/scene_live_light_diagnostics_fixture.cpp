#include "../src/scene_live_light_diagnostics.hpp"
#include <iostream>

int main() {
    using namespace t3render::live;
    VisualLight light;light.id="light:\"\\\n\xc3\xa8";light.generation=2;light.binding=BindingEvidence::Candidate;
    light.position={Vec3{1,2,3},BindingEvidence::Candidate};
    std::vector<VisualLight> lights(33,light);
    lights[1].position={};
    std::cout<<'{';
    // The writer appends fields to an existing record.
    std::cout<<"\"event\":\"frame\"";WriteLightDetails(std::cout,lights);std::cout<<'}';
}
