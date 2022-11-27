#define LUMIX_NO_CUSTOM_CRT
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/log.h"
#include "engine/os.h"
#include "engine/reflection.h"
#include "engine/stream.h"
#include "engine/universe.h"

#include "imgui/imgui.h"


using namespace Lumix;

static const u32 OUTPUT_FLAG = NodeEditor::OUTPUT_FLAG;

struct Variable {
	enum Type {
		NUMBER,

		COUNT
	};

	Variable(IAllocator& allocator) : name(allocator) {}

	String name;
	//Type type;
};

struct Graph;

struct Node : NodeEditorNode {
	enum class Type : u32 {
		ADD,
		SEQUENCE,
		SELF,
		SET_YAW,
		CONST,
		MOUSE_MOVE,
		UPDATE,
		GET_VARIABLE,
		SET_VARIABLE,
		SET_PROPERTY,
		MUL,
		CALL,
		VEC3,
		YAW_TO_DIR
	};

	bool nodeGUI() override {
		m_input_pin_counter = 0;
		m_output_pin_counter = 0;
		ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
		onGUI();
		ImGuiEx::EndNode();
		return false;
	}
	
	virtual Type getType() const = 0;

	virtual void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) = 0;
	virtual void serialize(OutputMemoryStream& blob) {}
	virtual void deserialize(InputMemoryStream& blob) {}
	virtual void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) {}

protected:
	struct NodeInput {
		Node* node;
		u32 input_idx;
	};

	NodeInput getOutputNode(u32 idx, const Graph& graph);

	struct NodeOutput {
		Node* node;
		u32 output_idx;
	};

	NodeOutput getInputNode(u32 idx, const Graph& graph);

	void inputPin() {
		ImGuiEx::Pin(m_id | (m_input_pin_counter << 16), true);
		++m_input_pin_counter;
	}

	void outputPin() {
		ImGuiEx::Pin(m_id | (m_output_pin_counter << 16) | OUTPUT_FLAG, false);
		++m_output_pin_counter;
	}

	void flowInput() {
		ImGuiEx::Pin(m_id | (m_input_pin_counter << 16), true, ImGuiEx::PinShape::TRIANGLE);
		++m_input_pin_counter;
	}

	void flowOutput() {
		ImGuiEx::Pin(m_id | (m_output_pin_counter << 16) | OUTPUT_FLAG, false, ImGuiEx::PinShape::TRIANGLE);
		++m_output_pin_counter;
	}

	virtual void onGUI() = 0;
	u32 m_input_pin_counter = 0;
	u32 m_output_pin_counter = 0;
	bool m_selected = false;
};


struct Graph {
	Graph(IAllocator& allocator)
		: m_allocator(allocator)
		, m_nodes(allocator)
		, m_links(allocator)
		, m_variables(allocator)
	{}

	static constexpr u32 MAGIC = '_LVS';

	bool deserialize(InputMemoryStream& blob) {
		const u32 magic = blob.read<u32>();
		if (magic != MAGIC) return false;
		const u32 version = blob.read<u32>();
		if (version != 0) return false;
		
		blob.read(node_counter_);
		const u32 var_count = blob.read<u32>();
		m_variables.reserve(var_count);
		for (u32 i = 0; i < var_count; ++i) {
			Variable& var = m_variables.emplace(m_allocator);
			var.name = blob.readString();
		}

		const u32 link_count = blob.read<u32>();
		m_links.reserve(link_count);
		for (u32 i = 0; i < link_count; ++i) {
			NodeEditorLink& link = m_links.emplace();
			blob.read(link);
		}

		const u32 node_count = blob.read<u32>();
		m_nodes.reserve(node_count);
		for (u32 i = 0; i < node_count; ++i) {
			const Node::Type type = blob.read<Node::Type>();
			Node* n = createNode(type);
			blob.read(n->m_id);
			blob.read(n->m_pos);
			n->deserialize(blob);
		}
		return true;
	}

	Node* createNode(Node::Type type);

	void serialize(OutputMemoryStream& blob) {
		blob.write(MAGIC);
		const u32 version = 0;
		blob.write(version);
		blob.write(node_counter_);
		
		blob.write(m_variables.size());
		for (const Variable& var : m_variables) {
			blob.writeString(var.name.c_str());
		}

		blob.write(m_links.size());
		for (const NodeEditorLink& link : m_links) {
			blob.write(link);
		}

		blob.write(m_nodes.size());
		for (const UniquePtr<Node>& node : m_nodes) {
			blob.write(node->getType());
			blob.write(node->m_id);
			blob.write(node->m_pos);
			node->serialize(blob);
		}
	}

	IAllocator& m_allocator;
	Array<UniquePtr<Node>> m_nodes;
	Array<NodeEditorLink> m_links;
	Array<Variable> m_variables;

	u32 node_counter_ = 0;
	template <typename T, typename... Args>
	Node* addNode(Args&&... args) {
		UniquePtr<T> n = UniquePtr<T>::create(m_allocator, static_cast<Args&&>(args)...);
		n->m_id = ++node_counter_;
		m_nodes.push(n.move());
		return m_nodes.last().get();
	}

	void removeNode(u32 node) {
		const u32 node_id = m_nodes[node]->m_id;
		for (i32 i = m_links.size() - 1; i >= 0; --i) {
			if ((m_links[i].from & 0x7fff) == node_id || (m_links[i].to & 0x7fff) == node_id) {
				m_links.erase(i);
			}	
		}
		m_nodes.erase(node);
	}

	void removeLink(u32 link) {
		m_links.erase(link);
	}

	Node* getNode(u32 id) const {
		const i32 idx = m_nodes.find([&](const UniquePtr<Node>& node){ return node->m_id == id; });
		return idx < 0 ? nullptr : m_nodes[idx].get();
	}
};

Node::NodeInput Node::getOutputNode(u32 idx, const Graph& graph) {
	const i32 i = graph.m_links.find([&](NodeEditorLink& l){
		return l.from == (m_id | (idx << 16) | OUTPUT_FLAG);
	});
	if (i == -1) return {nullptr, 0};

	const u32 to = graph.m_links[i].to;
	return { graph.getNode(to & 0x7fFF), to >> 16 };
}

Node::NodeOutput Node::getInputNode(u32 idx, const Graph& graph) {
	const i32 i = graph.m_links.find([&](NodeEditorLink& l){
		return l.to == (m_id | (idx << 16));
	});
	if (i == -1) return {nullptr, 0};

	const u32 from = graph.m_links[i].from;
	return { graph.getNode(from & 0x7fFF), from >> 16 };
}

struct SequenceNode : Node {
	Type getType() const override { return Type::SEQUENCE; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		flowInput(); ImGui::TextUnformatted(ICON_FA_LIST_OL);
		ImGui::SameLine();
		for (u32 i = 0; i < outputs_; ++i) {
			flowOutput();ImGui::NewLine();
		}
		if (ImGuiEx::IconButton(ICON_FA_PLUS, "Add")) ++outputs_;
	}
	
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}

	u32 outputs_ = 2;
};

struct SelfNode : Node {
	Type getType() const override { return Type::SELF; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }
	void onGUI() override {
		outputPin();
		ImGui::TextUnformatted("Self");
	}
	
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
	void printRef(OutputMemoryStream& blob, const Graph& graph, u32) override {
		blob << "this";
	}
};

struct CallNode : Node {
	CallNode() {}
	CallNode(reflection::ComponentBase* component, reflection::FunctionBase* function)
		: component(component)
		, function(function)
	{}

	void deserialize(InputMemoryStream& blob) override {
		const RuntimeHash cmp_name_hash = blob.read<RuntimeHash>();
		const char* func_name = blob.readString();
		const ComponentType cmp_type = reflection::getComponentTypeFromHash(cmp_name_hash);
		component = reflection::getComponent(cmp_type);
		if (component) {
			const i32 fi = component->functions.find([&](reflection::FunctionBase* func){
				return equalStrings(func->name, func_name);
			});
			if (fi < 0) {
				logError("Function not found"); // TODO proper error
			}
			else {
				function = component->functions[fi];
			}
		}
		else {
			logError("Component not found"); // TODO proper error
		}
	}

	void serialize(OutputMemoryStream& blob) override {
		blob.write(RuntimeHash(component->name));
		blob.writeString(function->name);
	}


	Type getType() const override { return Type::CALL; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		flowInput();
		ImGui::Text("%s.%s", component->name, function->name);
		ImGui::SameLine();
		flowOutput();
		ImGui::NewLine();
		for (u32 i = 0; i < function->getArgCount(); ++i) {
			inputPin(); ImGui::Text("Input %d", i);
		}
	}
	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {}

	const reflection::ComponentBase* component = nullptr;
	const reflection::FunctionBase* function = nullptr;
};

struct SetYawNode : Node {
	Type getType() const override { return Type::SET_YAW; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		flowInput(); ImGui::TextUnformatted("Set Yaw");
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		inputPin(); ImGui::TextUnformatted("Entity");
		inputPin(); ImGui::TextUnformatted("Yaw");
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		const NodeOutput entity_input = getInputNode(1, graph);
		const NodeOutput yaw_input = getInputNode(2, graph);
		if (!entity_input.node) return;
		if (!yaw_input.node) return;

		entity_input.node->generate(blob, graph, entity_input.output_idx);
		yaw_input.node->generate(blob, graph, yaw_input.output_idx);

		entity_input.node->printRef(blob, graph, entity_input.output_idx);
		blob << ".rotation = { 0, math.sin(";
		yaw_input.node->printRef(blob, graph, yaw_input.output_idx);
		blob << " * 0.5), 0, math.cos(";
		yaw_input.node->printRef(blob, graph, yaw_input.output_idx);
		blob << " * 0.5) }\n";
	}
};

struct ConstNode : Node {
	Type getType() const override { return Type::CONST; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		outputPin();
		ImGui::DragFloat("##v", &value_);
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {}
	void printRef(OutputMemoryStream& blob, const Graph& graph, u32) override {
		blob << value_;
	}

	float value_ = 0;
};

struct MouseMoveNode : Node {
	Type getType() const override { return Type::MOUSE_MOVE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		flowOutput(); ImGui::TextUnformatted(ICON_FA_MOUSE " Mouse move");
		outputPin(); ImGui::TextUnformatted("Delta X");
		outputPin(); ImGui::TextUnformatted("Delta Y");
	}
	
	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		switch(output_idx) {
			case 0: ASSERT(false); break;
			case 1: blob << "event.x"; break;
			case 2: blob << "event.y"; break;
		}
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		if (output_idx == 0) {
			blob << "function onInputEvent(event)\n";
			blob << "\tif event.type == LumixAPI.INPUT_EVENT_AXIS and event.device.type == LumixAPI.INPUT_DEVICE_MOUSE then\n";
			Node::NodeInput n = getOutputNode(0, graph);
			if (n.node) {
				n.node->generate(blob, graph, n.input_idx);
			}
			blob << "\tend\n";
			blob << "end\n";
		}
	}
};

struct Vec3Node : Node {
	Type getType() const override { return Type::VEC3; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::TextUnformatted("X");
		inputPin(); ImGui::TextUnformatted("Y");
		inputPin(); ImGui::TextUnformatted("Z");
		ImGui::EndGroup();
		ImGui::SameLine();
		outputPin();
	}
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
};

struct YawToDirNode : Node {
	Type getType() const override { return Type::YAW_TO_DIR; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		inputPin(); ImGui::TextUnformatted("Yaw to dir");
		ImGui::SameLine();
		outputPin();
	}
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
};

struct UpdateNode : Node {
	Type getType() const override { return Type::UPDATE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void onGUI() override {
		flowOutput();
		ImGui::TextUnformatted(ICON_FA_CLOCK "Update");
		outputPin();
		ImGui::TextUnformatted("Time delta");
	}

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
		blob << "function update(td)\n";
		//getOutput(0)->generate(blob);
		blob << "end\n";
	}
};


struct MulNode : Node {
	Type getType() const override { return Type::MUL; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0.node || !n1.node) return;

		n0.node->generate(blob, graph, n0.output_idx);
		n1.node->generate(blob, graph, n1.output_idx);

		blob << "local v" << m_id << " = ";
		n0.node->printRef(blob, graph, n0.output_idx);
		blob << " * ";
		n1.node->printRef(blob, graph, n0.output_idx);
		blob << "\n";
	}

	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		blob << "v" << m_id;
	}

	void onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::NewLine();
		inputPin(); ImGui::NewLine();
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::TextUnformatted("X");

		ImGui::SameLine();
		outputPin();
	}
};

struct AddNode : Node {
	Type getType() const override { return Type::ADD; }

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0.node || !n1.node) return;

		n0.node->generate(blob, graph, n0.output_idx);
		n1.node->generate(blob, graph, n1.output_idx);

		blob << "local v" << m_id << " = ";
		n0.node->printRef(blob, graph, n0.output_idx);
		blob << " + ";
		n1.node->printRef(blob, graph, n0.output_idx);
		blob << "\n";
	}

	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		blob << "v" << m_id;
	}

	void onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::NewLine();
		inputPin(); ImGui::NewLine();
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::TextUnformatted(ICON_FA_PLUS);

		ImGui::SameLine();
		outputPin();
	}
};

struct SetVariableNode : Node {
	SetVariableNode(Graph& graph) : m_graph(graph) {}
	SetVariableNode(Graph& graph, u32 var) : m_graph(graph), m_var(var) {}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	Type getType() const override { return Type::SET_VARIABLE; }

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		if (m_var >= (u32)graph.m_variables.size()) return;
		
		const NodeOutput n = getInputNode(1, graph);
		if (!n.node) return;

		n.node->generate(blob, graph, n.output_idx);

		blob << graph.m_variables[m_var].name.c_str() << " = ";
		n.node->printRef(blob, graph, n.output_idx);
		blob << "\n";

		const NodeInput on = getOutputNode(0, graph);
		if (!on.node) return;

		on.node->generate(blob, graph, on.input_idx);
	}

	void onGUI() override {
		const char* var_name = m_var < (u32)m_graph.m_variables.size() ? m_graph.m_variables[m_var].name.c_str() : "N/A";
		flowInput(); ImGui::Text("Set `%s`", var_name);
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		inputPin(); ImGui::TextUnformatted("Value");
	}

	Graph& m_graph;
	u32 m_var = 0;
};

struct GetVariableNode : Node {
	GetVariableNode(Graph& graph) : m_graph(graph) {}
	GetVariableNode(Graph& graph, u32 var) : m_graph(graph), m_var(var) {}
	Type getType() const override { return Type::GET_VARIABLE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
	
	void printRef(OutputMemoryStream& blob, const Graph& graph, u32) override {
		if (m_var >= (u32)m_graph.m_variables.size()) return;
		blob << m_graph.m_variables[m_var].name.c_str();
	}

	void onGUI() override {
		outputPin();
		const char* var_name = m_var < (u32)m_graph.m_variables.size() ? m_graph.m_variables[m_var].name.c_str() : "N/A";
		ImGui::Text(ICON_FA_PENCIL_ALT " %s", var_name);
	}

	Graph& m_graph;
	u32 m_var = 0;
};

struct SetPropertyNode : Node {
	Type getType() const override { return Type::SET_PROPERTY; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
//		getInput(1)->generate(blob);
		blob << "." << cmp << "." << prop << " = " << value;
	}

	void onGUI() override {
		flowInput(); ImGui::TextUnformatted("Set property");
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		
		inputPin();
		ImGui::TextUnformatted("Entity");

		ImGui::InputText("Component", cmp, sizeof(cmp));
		ImGui::InputText("Property", prop, sizeof(prop));
		
		inputPin();
		ImGui::InputText("Value", value, sizeof(value));
	}
	
	char prop[64] = {};
	char value[64] = {};
	char cmp[64] = {};
};

struct VisualScriptEditorPlugin : StudioApp::GUIPlugin {
	VisualScriptEditorPlugin (StudioApp& app) 
		: app_(app)
	{
		toggle_ui_.init("Visual Script Editor", "Toggle visual script editor", "visualScriptEditor", "", true);
		toggle_ui_.func.bind<&VisualScriptEditorPlugin::onAction>(this);
		toggle_ui_.is_selected.bind<&VisualScriptEditorPlugin::isOpen>(this);
		app.addWindowAction(&toggle_ui_);
		is_open_ = false;
		graph_.create(app.getAllocator());
	}
	
	~VisualScriptEditorPlugin() {
		app_.removeAction(&toggle_ui_);
	}

	void onAction() { is_open_ = !is_open_; }
	bool isOpen() const { return is_open_; }

	void onSettingsLoaded() override {
		is_open_ = app_.getSettings().getValue(Settings::GLOBAL, "is_visualscript_editor_open", false);
	}

	void onBeforeSettingsSaved() override {
		app_.getSettings().setValue(Settings::GLOBAL, "is_visualscript_editor_open", is_open_);
	}

	void generate() {
		char path[LUMIX_MAX_PATH];
		copyString(path, path_.c_str());
		if (!Path::replaceExtension(path, "lua")) return;

		OutputMemoryStream blob(app_.getAllocator());
		for (Variable& var : graph_->m_variables) {
			blob << "local " << var.name.c_str() << " = 0\n";
		}
		
		for (UniquePtr<Node>& node : graph_->m_nodes) {
			if (node->getType() == Node::Type::MOUSE_MOVE) {
				node->generate(blob, *graph_.get(), 0);
			}
		}
		
		os::OutputFile file;
		if (file.open(path)) {
			bool res = file.write(blob.data(), blob.size());
			if (!res) {
				logError("Could not write ", path);
			}
			file.close();
		}
	}

	void save(const char* path) {
		if (!path[0]) {
			saveAs();
			return;
		}
		OutputMemoryStream blob(app_.getAllocator());
		graph_->serialize(blob);

		os::OutputFile file;
		if (file.open(path)) {
			if (!file.write(blob.data(), blob.size())) {
				logError("Failed to write ", path);
			}
			else {
				path_ = path;
				//m_dirty = false;
			}
			file.close();
		}
		else {
			logError("Failed to open ", path);
		}
	}

	void saveAs() {
		char path[LUMIX_MAX_PATH];
		if (!os::getSaveFilename(Span(path), "Visual script\0*.lvs\0", "lvs")) return;

		save(path);
	}

	void load() {
		char path[LUMIX_MAX_PATH];
		if (!os::getOpenFilename(Span(path), "Visual script\0*.lvs\0", nullptr)) return;
		os::InputFile file;
		if (file.open(path)) {
			const u64 size = file.size();
			OutputMemoryStream blob(app_.getAllocator());
			blob.resize(size);
			if (!file.read(blob.getMutableData(), blob.size())) {
				logError("Failed to read ", path);
				file.close();
				return;
			}
			file.close();

			graph_.destroy();
			graph_.create(app_.getAllocator());
			if (graph_->deserialize(InputMemoryStream(blob))) {
				path_ = path;
				return;
			}

			graph_.destroy();
			graph_.create(app_.getAllocator());
		}
	}

	void newGraph() {
		graph_.destroy();
		graph_.create(app_.getAllocator());
		path_ = "";
	}

	void menu() {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("New")) newGraph();
				if (ImGui::MenuItem("Load")) load();
				//if (ImGui::MenuItem("Load from entity", nullptr, false, emitter)) loadFromEntity();
				if (ImGui::MenuItem("Generate")) generate();
				if (ImGui::MenuItem("Save", nullptr, false, !path_.isEmpty())) save(path_.c_str());
				if (ImGui::MenuItem("Save as")) saveAs();
				//ImGui::Separator();
			
				//menuItem(m_apply_action, emitter && emitter->getResource());
				//ImGui::MenuItem("Autoapply", nullptr, &m_autoapply, emitter && emitter->getResource());

				ImGui::EndMenu();
			}
			if (ImGuiEx::IconButton(ICON_FA_CHECK, "Generate")) generate();
			if (ImGuiEx::IconButton(ICON_FA_FOLDER_OPEN, "Load")) load();
			if (ImGuiEx::IconButton(ICON_FA_SAVE, "Save")) save(path_.c_str());
			ImGui::EndMenuBar();
		}
	}

	void onWindowGUI() override {
		if (!is_open_) return;

		i32 hovered_node = -1;
		i32 hovered_link = -1;
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Visual script", &is_open_, ImGuiWindowFlags_MenuBar)) {
			menu();
			ImGui::Columns(2);
			static bool once = [](){ ImGui::SetColumnWidth(-1, 150); return true; }();
			for (Variable& var : graph_->m_variables) {
				ImGui::PushID(&var);
				if (ImGuiEx::IconButton(ICON_FA_TRASH, "Delete")) {
					graph_->m_variables.erase(u32(&var - graph_->m_variables.begin()));
					ImGui::PopID();
					break;
				}
				ImGui::SameLine();
				char buf[128];
				copyString(buf, var.name.c_str());
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputText("##", buf, sizeof(buf))) {
					var.name = buf;
				}
				ImGui::PopID();
			}
			if (ImGui::Button(ICON_FA_PLUS " Add variable")) {
				graph_->m_variables.emplace(app_.getAllocator());
			}
			
			ImGui::NextColumn();
			static ImVec2 offset = ImVec2(0, 0);
			const ImVec2 editor_pos = ImGui::GetCursorScreenPos();
			ImGuiEx::BeginNodeEditor("vs", &offset);
			
			for (UniquePtr<Node>& node : graph_->m_nodes) {
				node->nodeGUI();
				if (ImGui::IsItemHovered()) {
					hovered_node = i32(&node - graph_->m_nodes.begin());
				}
			}

			for (const NodeEditorLink& link : graph_->m_links) {
				ImGuiEx::NodeLink(link.from, link.to);
				if (ImGuiEx::IsLinkHovered()) {
					hovered_link = i32(&link - graph_->m_links.begin());
				}
			}

			ImGuiID link_from, link_to;
			if (ImGuiEx::GetNewLink(&link_from, &link_to)) {
				NodeEditorLink& link = graph_->m_links.emplace();
				link.from = link_from;
				link.to = link_to;
			}

			ImGuiEx::EndNodeEditor();
			ImGui::Columns();
	
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
				ImGui::OpenPopup("context_menu");
				context_node_ = hovered_node;
				context_link_ = hovered_link;
			}

			contextMenu(editor_pos);
		}
		ImGui::End();
	}

	void contextMenu(const ImVec2& editor_pos) {
		if (!ImGui::BeginPopup("context_menu")) return;

		ImVec2 cp = ImGui::GetItemRectMin();
		if (ImGui::BeginMenu("Add node")) {
			Node* n = nullptr;
			if (ImGui::Selectable("Add")) n = graph_->addNode<AddNode>();
			if (ImGui::Selectable("Multiply")) n = graph_->addNode<MulNode>();
			if (ImGui::BeginMenu("Set variable")) {
				for (const Variable& var : graph_->m_variables) {
					if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
						n = graph_->addNode<SetVariableNode>(*graph_, u32(&var - graph_->m_variables.begin()));
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Get variable")) {
				for (const Variable& var : graph_->m_variables) {
					if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
						n = graph_->addNode<GetVariableNode>(*graph_, u32(&var - graph_->m_variables.begin()));
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::Selectable("Self")) n = graph_->addNode<SelfNode>();
			if (ImGui::Selectable("Set yaw")) n = graph_->addNode<SetYawNode>();
			if (ImGui::Selectable("Mouse move")) n = graph_->addNode<MouseMoveNode>();
			if (ImGui::Selectable("Constant")) n = graph_->addNode<ConstNode>();
			if (ImGui::Selectable("Set property")) n = graph_->addNode<SetPropertyNode>();
			if (ImGui::Selectable("Update")) n = graph_->addNode<UpdateNode>();
			if (ImGui::Selectable("Vector 3")) n = graph_->addNode<Vec3Node>();
			if (ImGui::Selectable("Yaw to direction")) n = graph_->addNode<YawToDirNode>();
			if (ImGui::Selectable("Sequence")) n = graph_->addNode<SequenceNode>();
			if (n) {
				n->m_pos = ImGui::GetMousePos() - editor_pos;
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Call")) {
			for (const reflection::RegisteredComponent& rcmp : reflection::getComponents()) {
				if (!rcmp.cmp->functions.empty() && ImGui::BeginMenu(rcmp.cmp->name)) {
					for (reflection::FunctionBase* f : rcmp.cmp->functions) {
						if (ImGui::Selectable(f->name)) {
							graph_->addNode<CallNode>(rcmp.cmp, f);
						}
					}
					ImGui::EndMenu();
				}
			}
			ImGui::EndMenu();
		}

		if (context_node_ != -1 && ImGui::Selectable("Remove node")) {
			graph_->removeNode(context_node_);
		}
		if (context_link_ != -1 && ImGui::Selectable("Remove link")) {
			graph_->removeLink(context_link_);
		}
		ImGui::EndPopup();
	}

	const char* getName() const override { return "visualscript"; }

	StudioApp& app_;
	Local<Graph> graph_;
	bool is_open_ = false;
	Path path_;
	Action toggle_ui_;
	i32 context_node_ = -1;
	i32 context_link_ = -1;
};

Node* Graph::createNode(Node::Type type) {
	switch (type) {
		case Node::Type::ADD: return addNode<AddNode>();
		case Node::Type::MUL: return addNode<MulNode>();
		case Node::Type::SEQUENCE: return addNode<SequenceNode>();
		case Node::Type::SELF: return addNode<SelfNode>();
		case Node::Type::SET_YAW: return addNode<SetYawNode>();
		case Node::Type::CONST: return addNode<ConstNode>();
		case Node::Type::MOUSE_MOVE: return addNode<MouseMoveNode>();
		case Node::Type::UPDATE: return addNode<UpdateNode>();
		case Node::Type::VEC3: return addNode<Vec3Node>();
		case Node::Type::CALL: return addNode<CallNode>();
		case Node::Type::GET_VARIABLE: return addNode<GetVariableNode>(*this);
		case Node::Type::SET_VARIABLE: return addNode<SetVariableNode>(*this);
		case Node::Type::SET_PROPERTY: return addNode<SetPropertyNode>();
		case Node::Type::YAW_TO_DIR: return addNode<YawToDirNode>();

		default: ASSERT(false); break;
	}
	return nullptr;
}

LUMIX_STUDIO_ENTRY(visualscript)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), VisualScriptEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
