#ifndef CLUSTERER_BINDLESS_H_
#define CLUSTERER_BINDLESS_H_

layout(std140, set = 0, binding = 2) uniform ClusterParameters
{
	ClustererParametersBindless cluster;
};

layout(std430, set = 1, binding = 6) readonly buffer ClustererData
{
	ClustererBindlessTransform cluster_transforms;
};

layout(std430, set = 1, binding = 7) readonly buffer ClustererBitmasks
{
	uint cluster_bitmask[];
};

#include "spot.h"
#include "point.h"

#endif