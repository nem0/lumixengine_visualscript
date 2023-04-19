#pragma once

#include "engine/plugin.h"
#include "../external/wasm3.h"

namespace Lumix {

enum class ScriptValueType : u32 {
	U32_DEPRECATED,
	I32,
	FLOAT,
	ENTITY
};

struct ScriptResource : Resource {
	static ResourceType TYPE;

	enum class Version : u32 {

		LAST
	};

	struct Header {
		static const u32 MAGIC = '_scr';

		u32 magic = MAGIC;
		Version version = Version::LAST;
	};

	ScriptResource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator);

	ResourceType getType() const override { return TYPE; }
	void unload() override;
	bool load(u64 size, const u8* mem) override;

	IAllocator& m_allocator;
	OutputMemoryStream m_bytecode;
};

struct Script {
	Script() {}
	Script(Script&& script);
	~Script();

	bool m_init_failed = false;
	IM3Runtime m_runtime = nullptr;
	IM3Module m_module = nullptr;
	ScriptResource* m_resource = nullptr;
};

struct ScriptScene : IScene {
	virtual Script& getScript(EntityRef entity) = 0;
};


} // namespace Lumix