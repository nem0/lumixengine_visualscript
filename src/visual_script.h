#pragma once

#include "engine/resource.h"

namespace Lumix {

struct VisualScript : Resource {
	VisualScript(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
		: Resource(path, resource_manager, allocator)
	{}

	ResourceType getType() const override { return TYPE; }
	void unload() override;
	bool load(u64 size, const u8* mem) override;

	static ResourceType TYPE;
};

} // namespace Lumix