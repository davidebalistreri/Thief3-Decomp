#include "../src/scene_live_selection_diagnostics.hpp"
#include <iostream>

using namespace t3render::live;
int main() {
    Snapshot snapshot;
    ResidentMesh mesh;
    mesh.selection.instance_id="actor:\"\\\n";
    mesh.selection.resource_id="mesh:bed";
    mesh.selection.generation=4;
    mesh.selection.binding=BindingEvidence::Candidate;
    mesh.selection.identity_scope=IdentityScope::VisibleList;
    mesh.selection.transform[0]=-0.0f;
    mesh.selection.transform[5]=1.0f;
    mesh.selection.sections={0,2};
    snapshot.meshes.push_back(mesh);
    ResidentBsp block;
    block.selection.resource_id="bsp:wall";
    block.selection.binding=BindingEvidence::Verified;
    block.selection.identity_scope=IdentityScope::FullObjectScan;
    block.selection.sections={1};
    snapshot.bsp.push_back(block);
    std::cout<<"{\"event\":\"frame\"";WriteSceneSelection(std::cout,snapshot);std::cout<<"}\n";
}
