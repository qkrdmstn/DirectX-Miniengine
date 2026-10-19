#include "../ProceduralMesh.h"

#include <cassert>
#include <cmath>
#include <limits>

using namespace MeshCutting;

static ProceduralMeshData Triangle()
{
    ProceduralMeshData data;
    data.vertices = {
        { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0 }, { 1, 0, 0, 1 } },
        { { 1, 0, 0 }, { 0, 0, 1 }, { 1, 0 }, { 0, 1, 0, 1 } },
        { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 1 }, { 0, 0, 1, 1 } }
    };
    data.indices = { 0, 1, 2 };
    return data;
}

int main()
{
    ProceduralMesh a, b;
    assert(a.SetMesh(Triangle()));
    assert(b.SetMesh(Triangle()));
    assert(a.GetData().sections.size() == 1);
    assert(a.GetBounds().valid);
    assert(a.GetBounds().min.x == 0.0f && a.GetBounds().max.x == 1.0f);
    const uint64_t vertexVersion = a.GetVertexVersion();
    const uint64_t topologyVersion = a.GetTopologyVersion();

    auto badIndex = Triangle();
    badIndex.indices[2] = 3;
    assert(!a.SetMesh(badIndex));
    assert(a.GetData().indices[2] == 2 && a.GetVertexVersion() == vertexVersion);

    auto badSections = Triangle();
    badSections.sections = { { 0, 3, 0 }, { 0, 3, 1 } };
    assert(!a.SetMesh(badSections));
    assert(a.GetTopologyVersion() == topologyVersion);

    auto badVertices = a.GetData().vertices;
    badVertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    assert(!a.UpdateVertices(badVertices));
    assert(a.GetBounds().min.x == 0.0f && a.GetVertexVersion() == vertexVersion);
    badVertices.pop_back();
    assert(!a.UpdateVertices(badVertices));

    auto changed = a.GetData().vertices;
    changed[0].position.x = -2.0f;
    assert(a.UpdateVertices(changed));
    assert(a.GetVertexVersion() > vertexVersion);
    assert(a.GetVertexVersion() != b.GetVertexVersion());
    assert(a.GetTopologyVersion() == topologyVersion);
    assert(a.GetBounds().min.x == -2.0f);
    assert(b.GetData().vertices[0].position.x == 0.0f);

    a.Clear();
    assert(a.GetData().indices.empty() && !a.GetBounds().valid);
    assert(a.GetTopologyVersion() > topologyVersion);
    assert(a.SetMesh(Triangle()));
    assert(a.GetData().indices.size() == 3);
}
