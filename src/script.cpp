#include "engine/hash_map.h"
#include "engine/engine.h"
#include "engine/input_system.h"
#include "engine/log.h"
#include "engine/plugin.h"
#include "engine/reflection.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/world.h"
#include "script.h"
#include "../external/wasm3.h"

namespace Lumix {

ResourceType ScriptResource::TYPE("script");
static const ComponentType SCRIPT_TYPE = reflection::getComponentType("script");

void ScriptResource::unload() {
	m_bytecode.clear();
}

ScriptResource::ScriptResource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_bytecode(allocator)
	, m_allocator(allocator)
{}

bool ScriptResource::load(u64 size, const u8* mem) {
	InputMemoryStream blob(mem, size);
	Header header;
	blob.read(header);
	if (header.magic != Header::MAGIC) return false;
	if (header.version > 0) return false;

	u32 bytecode_size = u32(blob.size() - blob.getPosition());
	m_bytecode.resize(bytecode_size);
	blob.read(m_bytecode.getMutableData(), bytecode_size);
	return true;
}

Script::Script(Script&& script)
{
	m_runtime = script.m_runtime;
	m_module = script.m_module;
	m_resource = script.m_resource;
	m_init_failed = script.m_init_failed;

	script.m_resource = nullptr;
	script.m_runtime = nullptr;
	script.m_module = nullptr;
}

Script::~Script() {
	if (m_resource) m_resource->decRefCount();
	ASSERT(!m_runtime);
	ASSERT(!m_module);
}

struct ScriptSceneImpl : ScriptScene {
	ScriptSceneImpl(IPlugin& plugin, Engine& engine, World& world, IAllocator& allocator)
		: m_plugin(plugin)
		, m_world(world)
		, m_engine(engine)
		, m_allocator(allocator)
		, m_scripts(allocator)
		, m_mouse_move_scripts(allocator)
		, m_key_input_scripts(allocator)
	{}

	void tryCall(EntityRef entity, const char* function_name, ...) {
		Script& scr = m_scripts[entity];
		va_list ap;
		va_start(ap, function_name);
		IM3Function fn;
		M3Result find_res = m3_FindFunction(&fn, scr.m_runtime, function_name);
		if (find_res == m3Err_none) {
			m3_CallVL(fn, ap);
		}
		else if (find_res != m3Err_functionLookupFailed) {
			logError(scr.m_resource->getPath(), ": ", find_res);
		}
		va_end(ap);
	}

	void serialize(OutputMemoryStream& blob) override {
		blob.write(m_scripts.size());
		for (auto iter = m_scripts.begin(), end = m_scripts.end(); iter != end; ++iter) {
			blob.write(iter.key());
			ScriptResource* res = iter.value().m_resource;
			blob.writeString(res ? res->getPath().c_str() : "");
		}
	}

	void deserialize(InputMemoryStream& blob, const EntityMap& entity_map, i32 version) override {
		u32 count;
		blob.read(count);
		ResourceManagerHub& rm = m_engine.getResourceManager();
		for (u32 i = 0; i < count; ++i) {
			EntityRef e;
			blob.read(e);
			e = entity_map.get(e);
			const char* path = blob.readString();
			Script script;
			script.m_resource = path[0] ? rm.load<ScriptResource>(Path(path)) : nullptr;
			m_scripts.insert(e, static_cast<Script&&>(script));
			m_world.onComponentCreated(e, SCRIPT_TYPE, this);
		}
	}

	IPlugin& getPlugin() const override { return m_plugin; }
	World& getWorld() override { return m_world; }
	void clear() override {}

	void stopGame() override {
		m_is_game_running = false;
		m_mouse_move_scripts.clear();
		m_key_input_scripts.clear();
	}

	void startGame() override {
		m_is_game_running = true;
		m_environment = m3_NewEnvironment();
	}

	void onKeyEvent(const InputSystem::Event& event) {
		for (EntityRef e : m_key_input_scripts) {
			tryCall(e, "onKeyEvent", event.data.button.key_id);
		}
	}

	void onMouseMove(const InputSystem::Event& event) {
		for (EntityRef e : m_mouse_move_scripts) {
			tryCall(e, "onMouseMove", event.data.axis.x, event.data.axis.y);
		}
	}

	static m3ApiRawFunction(API_getPropertyFloat) {
		/*m3ApiReturnType(float);
		ScriptSceneImpl* scene = (ScriptSceneImpl*)m3_GetUserData(runtime);
		World& world = scene->getWorld();
		m3ApiGetArg(EntityRef, entity);
		m3ApiGetArg(StableHash, property_hash);
		const reflection::PropertyBase* prop = reflection::getPropertyFromHash(property_hash);
		if (!prop) {
			logError("Property (hash = ", property_hash.getHashValue(), ") not found");
			return m3Err_none;
		}
		const reflection::Property<float>* fprop = static_cast<const reflection::Property<float>*>(prop);
		ComponentUID cmp;
		cmp.entity = entity;
		cmp.scene = world.getScene(prop->cmp->component_type);
		ASSERT(cmp.scene);
		const float value = fprop->get(cmp, -1);*/
		//m3ApiReturn(value);
		ASSERT(false); // TODO check if this function is correct
		return m3Err_none;

	}

	static m3ApiRawFunction(API_setPropertyFloat) {
		/*ScriptSceneImpl* scene = (ScriptSceneImpl*)m3_GetUserData(runtime);
		World& world = scene->getWorld();
		m3ApiGetArg(EntityRef, entity);
		m3ApiGetArg(StableHash, property_hash);
		m3ApiGetArg(float, value);
		const reflection::PropertyBase* prop = reflection::getPropertyFromHash(property_hash);
		if (!prop) {
			logError("Property (hash = ", property_hash.getHashValue(), ") not found");
			return m3Err_none;
		}
		const reflection::Property<float>* fprop = static_cast<const reflection::Property<float>*>(prop);
		ComponentUID cmp;
		cmp.entity = entity;
		cmp.scene = world.getScene(prop->cmp->component_type);
		ASSERT(cmp.scene);
		fprop->set(cmp, -1, value);*/
		ASSERT(false); // TODO check if this function is correct
		return m3Err_none;
	}

	static m3ApiRawFunction(API_setYaw) {
		ScriptSceneImpl* scene = (ScriptSceneImpl*)m3_GetUserData(runtime);
		World& world = scene->getWorld();
		m3ApiGetArg(EntityRef, entity);
		m3ApiGetArg(float, yaw);
		Quat rot(Vec3(0, 1, 0), yaw);
		world.setRotation(entity, rot);
		return m3Err_none;
	}

	void update(float time_delta, bool paused) override {
		if (paused) return;
		if (!m_is_game_running) return;
		
		InputSystem& input = m_engine.getInputSystem();
		const InputSystem::Event* events = input.getEvents();
		for (u32 i = 0, c = input.getEventsCount(); i < c; ++i) {
			switch(events[i].type) {
				case InputSystem::Event::BUTTON:
					if (events[i].device->type == InputSystem::Device::KEYBOARD) {
						onKeyEvent(events[i]);
					}
					break;
				case InputSystem::Event::AXIS:
					if (events[i].device->type == InputSystem::Device::MOUSE) {
						onMouseMove(events[i]);
					}
					break;
				default: break;
			}
		}

		for (auto iter = m_scripts.begin(), end = m_scripts.end(); iter != end; ++iter) {
			Script& script = iter.value();
			if (script.m_init_failed) continue;
			if (!script.m_resource) continue;
			if (!script.m_resource->isReady()) continue;

			bool start = false;
			if (!script.m_runtime) {
				script.m_runtime = m3_NewRuntime(m_environment, 32 * 1024, this);
				// TODO optimize - do not parse for each instance
				const M3Result parse_res = m3_ParseModule(m_environment, &script.m_module, script.m_resource->m_bytecode.data(), (u32)script.m_resource->m_bytecode.size());
				if (parse_res != m3Err_none) {
					logError(script.m_resource->getPath(), ": ", parse_res);
					// TODO reset m_init_failed if resource is reloaded
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_module = nullptr;
					script.m_runtime = nullptr;
					continue;
				}
				const M3Result load_res = m3_LoadModule(script.m_runtime, script.m_module);
				if (load_res != m3Err_none) {
					logError(script.m_resource->getPath(), ": ", load_res);
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_module = nullptr;
					script.m_runtime = nullptr;
					continue;
				}

				#define LINK(F) \
					{ \
						const M3Result link_res = m3_LinkRawFunction(script.m_module, "LumixAPI", #F, nullptr, &ScriptSceneImpl::API_##F); \
						if (link_res != m3Err_none) { \
							logError(script.m_resource->getPath(), ": " #F ": ", link_res); \
							script.m_init_failed = true; \
							m3_FreeRuntime(script.m_runtime); \
							script.m_module = nullptr; \
							script.m_runtime = nullptr; \
							continue; \
						} \
					}

				LINK(setYaw);
				LINK(setPropertyFloat);
				LINK(getPropertyFloat);

				#undef LINK

				IM3Global self_global = m3_FindGlobal(script.m_module, "self");
				if (!self_global) {
					logError(script.m_resource->getPath(), ": `self` not found");
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_module = nullptr;
					script.m_runtime = nullptr;
					continue;
				}
				
				M3TaggedValue self_value;
				self_value.type = c_m3Type_i32;
				self_value.value.i32 = iter.key().index;
				M3Result set_self_res = m3_SetGlobal(self_global, &self_value);
				if (set_self_res != m3Err_none) {
					logError(script.m_resource->getPath(), ": ", set_self_res);
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_module = nullptr;
					script.m_runtime = nullptr;
					continue;
				}
			}

			IM3Function tmp_fn;
			if (m3_FindFunction(&tmp_fn, script.m_runtime, "onMouseMove") == m3Err_none) {
				m_mouse_move_scripts.push(iter.key());
			}
			if (m3_FindFunction(&tmp_fn, script.m_runtime, "onKeyEvent") == m3Err_none) {
				m_key_input_scripts.push(iter.key());
			}

			IM3Function update_fn;
			M3Result find_update_res = m3_FindFunction(&update_fn, script.m_runtime, "update");
			if (find_update_res == m3Err_none) {
				m3_CallV(update_fn, time_delta);
			}
			else if (find_update_res != m3Err_functionLookupFailed) {
				logError(script.m_resource->getPath(), ": ", find_update_res);
				script.m_init_failed = true;
			}
		}
	}

	void destroyScript(EntityRef entity) {
		m_mouse_move_scripts.eraseItem(entity);
		m_key_input_scripts.eraseItem(entity);
		m_scripts.erase(entity);
		m_world.onComponentDestroyed(entity, SCRIPT_TYPE, this);
	}

	void createScript(EntityRef entity) {
		m_scripts.insert(entity);
		m_world.onComponentCreated(entity, SCRIPT_TYPE, this);
	}

	Script& getScript(EntityRef entity) override {
		return m_scripts[entity];
	}

	void setScriptResource(EntityRef entity, const Path& path) {
		Script& script = m_scripts[entity];
		if (script.m_resource) script.m_resource->decRefCount();
		if (path.isEmpty()) {
			script.m_resource = nullptr;
			return;
		}
		script.m_resource = m_engine.getResourceManager().load<ScriptResource>(path);
	}

	Path getScriptResource(EntityRef entity) {
		Script& script = m_scripts[entity];
		ScriptResource* res = script.m_resource;
		return res ? res->getPath() : Path();
	}

	IAllocator& m_allocator;
	Engine& m_engine;
	IPlugin& m_plugin;
	World& m_world;
	HashMap<EntityRef, Script> m_scripts;
	Array<EntityRef> m_mouse_move_scripts;
	Array<EntityRef> m_key_input_scripts;
	bool m_is_game_running = false;
	IM3Environment m_environment = nullptr;
};

struct ScriptManager : ResourceManager {
	ScriptManager(IAllocator& allocator)
		: ResourceManager(allocator)
	{}

	Resource* createResource(const Path& path) override {
		return LUMIX_NEW(m_allocator, ScriptResource)(path, *this, m_allocator);
	}

	void destroyResource(Resource& resource) override {
		LUMIX_DELETE(m_allocator, &resource);
	}
};

struct VisualScriptPlugin : IPlugin {
	VisualScriptPlugin(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
		, m_script_manager(engine.getAllocator())
	{
		OutputMemoryStream wasm_bin(engine.getAllocator());
		(void)engine.getFileSystem().getContentSync(Path("test.wasm"), wasm_bin);

		m_script_manager.create(ScriptResource::TYPE, engine.getResourceManager());
	
		LUMIX_SCENE(ScriptSceneImpl, "script")
			.LUMIX_CMP(Script, "script", "Script")
				.LUMIX_PROP(ScriptResource, "Script").resourceAttribute(ScriptResource::TYPE);
	}

	const char* getName() const override { return "script"; }
	u32 getVersion() const override { return 0; }
	void serialize(OutputMemoryStream& serializer) const override {}
	bool deserialize(u32 version, InputMemoryStream& serializer) override { return true; }

	void createScenes(World& world) override {
		UniquePtr<ScriptScene> scene = UniquePtr<ScriptSceneImpl>::create(m_allocator, *this, m_engine, world, m_allocator);
		world.addScene(scene.move());
	}

	IAllocator& m_allocator;
	Engine& m_engine;
	ScriptManager m_script_manager;
};

LUMIX_PLUGIN_ENTRY(visualscript) {
	return LUMIX_NEW(engine.getAllocator(), VisualScriptPlugin)(engine);
}

} // namespace Lumix

