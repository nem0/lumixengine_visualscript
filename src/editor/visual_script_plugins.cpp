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
		YAW_TO_DIR,
		START
	};

	bool nodeGUI() override {
		m_input_pin_counter = 0;
		m_output_pin_counter = 0;
		ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
		bool res = onGUI();
		ImGuiEx::EndNode();
		return res;
	}
	
	void nodeTitle(const char* title, bool input_flow, bool output_flow) {
		ImGuiEx::BeginNodeTitleBar();
		if (input_flow) flowInput();
		if (output_flow) flowOutput();
		ImGui::TextUnformatted(title);
		ImGuiEx::EndNodeTitleBar();
	}

	virtual Type getType() const = 0;

	virtual void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) = 0;
	virtual void serialize(OutputMemoryStream& blob) const {}
	virtual void deserialize(InputMemoryStream& blob) {}
	virtual void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) {}

	bool m_selected = false;
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

	virtual bool onGUI() = 0;
	u32 m_input_pin_counter = 0;
	u32 m_output_pin_counter = 0;
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
		for (const Node* node : m_nodes) {
			blob.write(node->getType());
			blob.write(node->m_id);
			blob.write(node->m_pos);
			node->serialize(blob);
		}
	}

	IAllocator& m_allocator;
	Array<Node*> m_nodes;
	Array<NodeEditorLink> m_links;
	Array<Variable> m_variables;

	u32 node_counter_ = 0;
	template <typename T, typename... Args>
	Node* addNode(Args&&... args) {
		Node* n = LUMIX_NEW(m_allocator, T)(static_cast<Args&&>(args)...);
		n->m_id = ++node_counter_;
		m_nodes.push(n);
		return n;
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
		const i32 idx = m_nodes.find([&](const Node* node){ return node->m_id == id; });
		return idx < 0 ? nullptr : m_nodes[idx];
	}
};

Node::NodeInput Node::getOutputNode(u32 idx, const Graph& graph) {
	const i32 i = graph.m_links.find([&](NodeEditorLink& l){
		return l.getFromNode() == m_id && l.getFromPin() == idx;
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
	SequenceNode(Graph& graph) : m_graph(graph) {}
	Type getType() const override { return Type::SEQUENCE; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		flowInput(); ImGui::TextUnformatted(ICON_FA_LIST_OL);
		ImGui::SameLine();
		u32 count = 0;
		for (const NodeEditorLink& link : m_graph.m_links) {
			if (link.getFromNode() == m_id) count = maximum(count, link.getFromPin() + 1);
		}
		for (u32 i = 0; i < count; ++i) {
			flowOutput();ImGui::NewLine();
		}
		flowOutput();ImGui::NewLine();
		return false;
	}
	
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
	Graph& m_graph;
};

struct SelfNode : Node {
	Type getType() const override { return Type::SELF; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }
	bool onGUI() override {
		outputPin();
		ImGui::TextUnformatted("Self");
		return false;
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

	void serialize(OutputMemoryStream& blob) const override {
		blob.write(RuntimeHash(component->name));
		blob.writeString(function->name);
	}


	Type getType() const override { return Type::CALL; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		flowInput();
		ImGui::Text("%s.%s", component->name, function->name);
		ImGui::SameLine();
		flowOutput();
		ImGui::NewLine();
		for (u32 i = 0; i < function->getArgCount(); ++i) {
			inputPin(); ImGui::Text("Input %d", i);
		}
		return false;
	}
	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {}

	const reflection::ComponentBase* component = nullptr;
	const reflection::FunctionBase* function = nullptr;
};

struct SetYawNode : Node {
	Type getType() const override { return Type::SET_YAW; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle("Set entity yaw", true, true);
		inputPin(); ImGui::TextUnformatted("Entity");
		inputPin(); ImGui::TextUnformatted("Yaw");
		return false;
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

	bool onGUI() override {
		outputPin();
		return ImGui::DragFloat("##v", &value_);
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

	bool onGUI() override {
		nodeTitle(ICON_FA_MOUSE " Mouse move", false, true);
		outputPin(); ImGui::TextUnformatted("Delta X");
		outputPin(); ImGui::TextUnformatted("Delta Y");
		return false;
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

	bool onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::TextUnformatted("X");
		inputPin(); ImGui::TextUnformatted("Y");
		inputPin(); ImGui::TextUnformatted("Z");
		ImGui::EndGroup();
		ImGui::SameLine();
		outputPin();
		return false;
	}
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
};

struct YawToDirNode : Node {
	Type getType() const override { return Type::YAW_TO_DIR; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		inputPin(); ImGui::TextUnformatted("Yaw to dir");
		ImGui::SameLine();
		outputPin();
		return false;
	}

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
};

struct StartNode : Node {
	Type getType() const override { return Type::START; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle(ICON_FA_PLAY "Start", false, true);
		return false;
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 pin_idx) override {
		if (pin_idx == 0) {
			blob << "function start()\n";
			NodeInput o = getOutputNode(0, graph);
			if (o.node) o.node->generate(blob, graph, o.input_idx);
			blob << "end\n";
		}
	}
};

struct UpdateNode : Node {
	Type getType() const override { return Type::UPDATE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle(ICON_FA_CLOCK "Update", false, true);
		outputPin();
		ImGui::TextUnformatted("Time delta");
		return false;
	}

	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		blob << "td";
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 pin_idx) override {
		if (pin_idx == 0) {
			blob << "function update(td)\n";
			NodeInput o = getOutputNode(0, graph);
			if(o.node) o.node->generate(blob, graph, o.input_idx);
			blob << "end\n";
		}
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

	bool onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::NewLine();
		inputPin(); ImGui::NewLine();
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::TextUnformatted("X");

		ImGui::SameLine();
		outputPin();
		return false;
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
	}

	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0.node || !n1.node) return;
		n0.node->printRef(blob, graph, n0.output_idx);
		blob << " + ";
		n1.node->printRef(blob, graph, n0.output_idx);
	}

	bool onGUI() override {
		ImGui::BeginGroup();
		inputPin(); ImGui::NewLine();
		inputPin(); ImGui::NewLine();
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::TextUnformatted(ICON_FA_PLUS);

		ImGui::SameLine();
		outputPin();
		return false;
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

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		flowInput();
		flowOutput();
		const char* var_name = m_var < (u32)m_graph.m_variables.size() ? m_graph.m_variables[m_var].name.c_str() : "N/A";
		ImGui::Text("Set " ICON_FA_PENCIL_ALT " %s", var_name);
		ImGuiEx::EndNodeTitleBar();

		inputPin(); ImGui::TextUnformatted("Value");
		return false;
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

	bool onGUI() override {
		outputPin();
		const char* var_name = m_var < (u32)m_graph.m_variables.size() ? m_graph.m_variables[m_var].name.c_str() : "N/A";
		ImGui::Text(ICON_FA_PENCIL_ALT " %s", var_name);
		return false;
	}

	Graph& m_graph;
	u32 m_var = 0;
};

struct SetPropertyNode : Node {
	Type getType() const override { return Type::SET_PROPERTY; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	static bool propertyInput(const char* label, ComponentType* type, Span<char> property_name) {
		bool res = false;
		StaticString<128> preview;
		if (*type == INVALID_COMPONENT_TYPE) preview = "Not set";
		else {
			preview = reflection::getComponent(*type)->name;
			preview.add(".");
			preview.add(property_name);
		}
		if (ImGui::BeginCombo(label, preview)) {
			static char filter[32] = "";
			ImGui::SetNextItemWidth(150);
			ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
			for (const reflection::RegisteredComponent& cmp : reflection::getComponents()) {
				struct : reflection::IEmptyPropertyVisitor {
					void visit(const reflection::Property<float>& prop) override {
						StaticString<128> tmp(cmp_name, ".", prop.name);
						if ((!filter[0] || stristr(tmp, filter)) && ImGui::Selectable(tmp)) {
							selected = true;
							copyString(property_name, prop.name);
						}
					}
					const char* filter;
					const char* cmp_name;
					bool selected = false;
					Span<char> property_name;
				} visitor;
				visitor.filter = filter;
				visitor.property_name = property_name;
				visitor.cmp_name = cmp.cmp->name;
				cmp.cmp->visit(visitor);
				if (visitor.selected) {
					res = visitor.selected;
					*type = cmp.cmp->component_type;
				}
			}
			ImGui::EndCombo();
		}
		return res;
	}

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
		// TODO name to propertyname 
		blob << "." << reflection::getComponent(cmp_type)->name << "." << prop << " = " << value;
	}

	bool onGUI() override {
		nodeTitle("Set property", true, true);
		
		inputPin();
		ImGui::TextUnformatted("Entity");
		ImGui::PushItemWidth(150);
		bool res = propertyInput("Property", &cmp_type, Span(prop));
		inputPin();
		res = ImGui::InputText("Value", value, sizeof(value)) || res;
		ImGui::PopItemWidth();
		return res;
	}
	
	char prop[64] = {};
	char value[64] = {};
	ComponentType cmp_type = INVALID_COMPONENT_TYPE;
};

struct VisualScriptEditorPlugin : StudioApp::GUIPlugin, NodeEditor {
	VisualScriptEditorPlugin (StudioApp& app) 
		: NodeEditor(app.getAllocator())	
		, m_app(app)
		, m_recent_paths(app.getAllocator())
	{
		m_toggle_ui.init("Visual Script Editor", "Toggle visual script editor", "visualScriptEditor", "", true);
		m_toggle_ui.func.bind<&VisualScriptEditorPlugin::onToggleUI>(this);
		m_toggle_ui.is_selected.bind<&VisualScriptEditorPlugin::isOpen>(this);
		
		m_save_action.init(ICON_FA_SAVE "Save", "Visual script save", "visual_script_editor_save", ICON_FA_SAVE, os::Keycode::S, Action::Modifiers::CTRL, true);
		m_save_action.func.bind<&VisualScriptEditorPlugin::save>(this);
		m_save_action.plugin = this;
		
		m_generate_action.init(ICON_FA_CHECK "Generate", "Visual script generate", "visual_script_editor_generate", ICON_FA_CHECK, os::Keycode::E, Action::Modifiers::CTRL, true);
		m_generate_action.func.bind<&VisualScriptEditorPlugin::generate>(this);
		m_generate_action.plugin = this;
		
		m_undo_action.init(ICON_FA_UNDO "Undo", "Visual script undo", "visual_script_editor_undo", ICON_FA_UNDO, os::Keycode::Z, Action::Modifiers::CTRL, true);
		m_undo_action.func.bind<&VisualScriptEditorPlugin::undo>((SimpleUndoRedo*)this);
		m_undo_action.plugin = this;

		m_redo_action.init(ICON_FA_REDO "Redo", "Visual script redo", "visual_script_editor_redo", ICON_FA_REDO, os::Keycode::Z, Action::Modifiers::CTRL | Action::Modifiers::SHIFT, true);
		m_redo_action.func.bind<&VisualScriptEditorPlugin::redo>((SimpleUndoRedo*)this);
		m_redo_action.plugin = this;

		m_delete_action.init(ICON_FA_TRASH "Delete", "Visual script delete", "visual_script_editor_delete", ICON_FA_TRASH, os::Keycode::DEL, Action::Modifiers::NONE, true);
		m_delete_action.func.bind<&VisualScriptEditorPlugin::deleteSelectedNodes>(this);
		m_delete_action.plugin = this;
		
		app.addAction(&m_save_action);
		app.addAction(&m_generate_action);
		app.addAction(&m_undo_action);
		app.addAction(&m_redo_action);
		app.addAction(&m_delete_action);
		app.addWindowAction(&m_toggle_ui);
		newGraph();
	}
	
	~VisualScriptEditorPlugin() {
		m_app.removeAction(&m_toggle_ui);
		m_app.removeAction(&m_save_action);
		m_app.removeAction(&m_generate_action);
		m_app.removeAction(&m_undo_action);
		m_app.removeAction(&m_redo_action);
		m_app.removeAction(&m_delete_action);
	}

	bool hasFocus() override { return m_has_focus; }
	
	void deleteSelectedNodes() {
		for (i32 i = m_graph->m_nodes.size() - 1; i >= 0; --i) {
			Node* node = m_graph->m_nodes[i];
			if (node->m_selected) {
				for (i32 j = m_graph->m_links.size() - 1; j >= 0; --j) {
					if (m_graph->m_links[j].getFromNode() == node->m_id || m_graph->m_links[j].getToNode() == node->m_id) {
						m_graph->m_links.erase(j);
					}
				}

				LUMIX_DELETE(m_graph->m_allocator, node);
				m_graph->m_nodes.swapAndPop(i);
			}
		}
		pushUndo(NO_MERGE_UNDO);
	}

	void onCanvasClicked(ImVec2 pos, i32 hovered_link) override {
		static struct {
			char key;
			const char* label;
			Node::Type type;
		} TYPES[] = {
			{ '1', "Const", Node::Type::CONST },
			{ '3', "Vec3", Node::Type::VEC3 },
			{ 'A', "Add", Node::Type::ADD },
			{ 'C', "Call", Node::Type::MUL },
			{ 'M', "Multiply", Node::Type::MUL },
			{ 'T', "Self", Node::Type::SELF },
			{ 'S', "Sequence", Node::Type::SEQUENCE },
			{ 'P', "Set property", Node::Type::SET_PROPERTY },
		};

		for (const auto& t : TYPES) {
			if (os::isKeyDown((os::Keycode)t.key)) {
				Node* n = m_graph->createNode(t.type);
				n->m_pos = pos;
				if (hovered_link >= 0) splitLink(m_graph->m_nodes.back(), m_graph->m_links, hovered_link);
				pushUndo(NO_MERGE_UNDO);
				break;
			}
		}
	}
	
	void onLinkDoubleClicked(NodeEditorLink& link, ImVec2 pos) override {}
	
	void deserialize(InputMemoryStream& blob) override {
		m_graph.destroy();
		m_graph.create(m_app.getAllocator());
		m_graph->deserialize(blob);
	}

	void serialize(OutputMemoryStream& blob) override {
		m_graph->serialize(blob);
	}

	void onToggleUI() { m_is_open = !m_is_open; }
	bool isOpen() const { return m_is_open; }

	void onSettingsLoaded() override {
		Settings& settings = m_app.getSettings();
		
		m_is_open = settings.getValue(Settings::GLOBAL, "is_visualscript_editor_open", false);
		m_recent_paths.clear();
		char tmp[LUMIX_MAX_PATH];
		for (u32 i = 0; ; ++i) {
			const StaticString<32> key("visual_script_editor_recent_", i);
			const u32 len = settings.getValue(Settings::LOCAL, key, Span(tmp));
			if (len == 0) break;
			m_recent_paths.emplace(tmp, m_app.getAllocator());
		}
	}

	void onBeforeSettingsSaved() override {
		Settings& settings = m_app.getSettings();
		settings.setValue(Settings::GLOBAL, "is_visualscript_editor_open", m_is_open);
				
		for (const String& p : m_recent_paths) {
			const u32 i = u32(&p - m_recent_paths.begin());
			const StaticString<32> key("visual_script_editor_recent_", i);
			settings.setValue(Settings::LOCAL, key, p.c_str());
		}
	}

	void generate() {
		char path[LUMIX_MAX_PATH];
		copyString(path, m_path.c_str());
		if (!Path::replaceExtension(path, "lua")) return;

		OutputMemoryStream blob(m_app.getAllocator());
		for (Variable& var : m_graph->m_variables) {
			blob << "local " << var.name.c_str() << " = 0\n";
		}
		
		for (Node* node : m_graph->m_nodes) {
			switch (node->getType()) {
				case Node::Type::START:
				case Node::Type::UPDATE:
				case Node::Type::MOUSE_MOVE:
					node->generate(blob, *m_graph.get(), 0);
					break;
				default: break;
			}
		}
		
		os::OutputFile file;
		FileSystem& fs = m_app.getEngine().getFileSystem();
		if (!fs.saveContentSync(Path(path), blob)) {
			logError("Could not save ", path);
		}
	}

	void save() {
		if (m_path.isEmpty()) m_show_save_as = true;
		else saveAs(m_path.c_str());
	}

	void saveAs(const char* path) {
		ASSERT(path[0]);
		OutputMemoryStream blob(m_app.getAllocator());
		m_graph->serialize(blob);
		FileSystem& fs = m_app.getEngine().getFileSystem();
		if (!fs.saveContentSync(Path(path), blob)) {
			logError("Failed to save ", path);
		}
		else {
			setPath(path);
		}
	}

	void setPath(const char* path) {
		m_path = path;
		String p(path, m_app.getAllocator());
		m_recent_paths.eraseItems([&](const String& s) { return s == path; });
		m_recent_paths.push(static_cast<String&&>(p));
	}

	void load(const char* path) {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		OutputMemoryStream blob(m_app.getAllocator());
		if (!fs.getContentSync(Path(path), blob)) {
			logError("Failed to read ", path);
			return;
		}

		m_graph.destroy();
		m_graph.create(m_app.getAllocator());
		if (m_graph->deserialize(InputMemoryStream(blob))) {
			pushUndo(NO_MERGE_UNDO);
			setPath(path);
			return;
		}

		m_graph.destroy();
		m_graph.create(m_app.getAllocator());
		pushUndo(NO_MERGE_UNDO);
	}

	void newGraph() {
		if (m_graph.get()) m_graph.destroy();
		m_graph.create(m_app.getAllocator());
		m_path = "";
		m_graph->addNode<UpdateNode>();
		pushUndo(NO_MERGE_UNDO);
	}

	void menu() {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("New")) newGraph();
				if (ImGui::MenuItem("Open")) m_show_open = true;
				menuItem(m_generate_action, true);
				menuItem(m_save_action, true);
				if (ImGui::MenuItem("Save as")) m_show_save_as = true;
				if (ImGui::BeginMenu("Recent", !m_recent_paths.empty())) {
					for (const String& s : m_recent_paths) {
						if (ImGui::MenuItem(s.c_str())) load(s.c_str());
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit")) {
				menuItem(m_undo_action, canUndo());
				menuItem(m_redo_action, canRedo());
				ImGui::EndMenu();
			}
			if (ImGuiEx::IconButton(ICON_FA_CHECK, "Generate")) generate();
			if (ImGuiEx::IconButton(ICON_FA_FOLDER_OPEN, "Open")) m_show_open = true;
			if (ImGuiEx::IconButton(ICON_FA_SAVE, "Save")) save();
			ImGui::EndMenuBar();
		}

		FileSelector& fs = m_app.getFileSelector();
		if (fs.gui("Open", &m_show_open, "lvs", false)) load(fs.getPath());
		if (fs.gui("Save As", &m_show_save_as, "lvs", true)) saveAs(fs.getPath());
	}

	void onWindowGUI() override {
		m_has_focus = false;
		if (!m_is_open) return;

		i32 hovered_node = -1;
		i32 hovered_link = -1;
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Visual script", &m_is_open, ImGuiWindowFlags_MenuBar)) {
			m_has_focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			menu();
			ImGui::Columns(2);
			static bool once = [](){ ImGui::SetColumnWidth(-1, 150); return true; }();
			for (Variable& var : m_graph->m_variables) {
				ImGui::PushID(&var);
				if (ImGuiEx::IconButton(ICON_FA_TRASH, "Delete")) {
					m_graph->m_variables.erase(u32(&var - m_graph->m_variables.begin()));
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
				m_graph->m_variables.emplace(m_app.getAllocator());
			}
			
			ImGui::NextColumn();
			static ImVec2 offset = ImVec2(0, 0);
			const ImVec2 editor_pos = ImGui::GetCursorScreenPos();
			nodeEditorGUI(m_graph->m_nodes, m_graph->m_links);
			/*ImGuiEx::BeginNodeEditor("vs", &offset);
			
			for (UniquePtr<Node>& node : m_graph->m_nodes) {
				node->nodeGUI();
				if (ImGui::IsItemHovered()) {
					hovered_node = i32(&node - m_graph->m_nodes.begin());
				}
			}

			for (const NodeEditorLink& link : m_graph->m_links) {
				ImGuiEx::NodeLink(link.from, link.to);
				if (ImGuiEx::IsLinkHovered()) {
					hovered_link = i32(&link - m_graph->m_links.begin());
				}
			}

			ImGuiID link_from, link_to;
			if (ImGuiEx::GetNewLink(&link_from, &link_to)) {
				NodeEditorLink& link = m_graph->m_links.emplace();
				link.from = link_from;
				link.to = link_to;
			}

			ImGuiEx::EndNodeEditor();*/
			ImGui::Columns();
		}
		ImGui::End();
	}

	void onContextMenu(bool recently_opened, ImVec2 pos) override {
		ImVec2 cp = ImGui::GetItemRectMin();
		if (ImGui::BeginMenu("Add node")) {
			Node* n = nullptr;
			if (ImGui::Selectable("Add")) n = m_graph->addNode<AddNode>();
			if (ImGui::Selectable("Multiply")) n = m_graph->addNode<MulNode>();
			if (ImGui::BeginMenu("Set variable")) {
				for (const Variable& var : m_graph->m_variables) {
					if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
						n = m_graph->addNode<SetVariableNode>(*m_graph, u32(&var - m_graph->m_variables.begin()));
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Get variable")) {
				for (const Variable& var : m_graph->m_variables) {
					if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
						n = m_graph->addNode<GetVariableNode>(*m_graph, u32(&var - m_graph->m_variables.begin()));
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::Selectable("Self")) n = m_graph->addNode<SelfNode>();
			if (ImGui::Selectable("Set yaw")) n = m_graph->addNode<SetYawNode>();
			if (ImGui::Selectable("Mouse move")) n = m_graph->addNode<MouseMoveNode>();
			if (ImGui::Selectable("Constant")) n = m_graph->addNode<ConstNode>();
			if (ImGui::Selectable("Set property")) n = m_graph->addNode<SetPropertyNode>();
			if (ImGui::Selectable("Update")) n = m_graph->addNode<UpdateNode>();
			if (ImGui::Selectable("Vector 3")) n = m_graph->addNode<Vec3Node>();
			if (ImGui::Selectable("Yaw to direction")) n = m_graph->addNode<YawToDirNode>();
			if (ImGui::Selectable("Sequence")) n = m_graph->addNode<SequenceNode>(*m_graph);
			if (ImGui::Selectable("Start")) n = m_graph->addNode<StartNode>();
			if (n) {
				n->m_pos = pos;
				pushUndo(NO_MERGE_UNDO);
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Call")) {
			for (const reflection::RegisteredComponent& rcmp : reflection::getComponents()) {
				if (!rcmp.cmp->functions.empty() && ImGui::BeginMenu(rcmp.cmp->name)) {
					for (reflection::FunctionBase* f : rcmp.cmp->functions) {
						if (ImGui::Selectable(f->name)) {
							m_graph->addNode<CallNode>(rcmp.cmp, f);
							pushUndo(NO_MERGE_UNDO);
						}
					}
					ImGui::EndMenu();
				}
			}
			ImGui::EndMenu();
		}
	}

	const char* getName() const override { return "visualscript"; }

	StudioApp& m_app;
	Local<Graph> m_graph;
	bool m_is_open = false;
	Path m_path;
	Action m_toggle_ui;
	Action m_save_action;
	Action m_generate_action;
	Action m_undo_action;
	Action m_redo_action;
	Action m_delete_action;
	Array<String> m_recent_paths;
	bool m_show_save_as = false;
	bool m_show_open = false;
	bool m_has_focus = false;
};

Node* Graph::createNode(Node::Type type) {
	switch (type) {
		case Node::Type::ADD: return addNode<AddNode>();
		case Node::Type::MUL: return addNode<MulNode>();
		case Node::Type::SEQUENCE: return addNode<SequenceNode>(*this);
		case Node::Type::SELF: return addNode<SelfNode>();
		case Node::Type::SET_YAW: return addNode<SetYawNode>();
		case Node::Type::CONST: return addNode<ConstNode>();
		case Node::Type::MOUSE_MOVE: return addNode<MouseMoveNode>();
		case Node::Type::START: return addNode<StartNode>();
		case Node::Type::UPDATE: return addNode<UpdateNode>();
		case Node::Type::VEC3: return addNode<Vec3Node>();
		case Node::Type::CALL: return addNode<CallNode>();
		case Node::Type::GET_VARIABLE: return addNode<GetVariableNode>(*this);
		case Node::Type::SET_VARIABLE: return addNode<SetVariableNode>(*this);
		case Node::Type::SET_PROPERTY: return addNode<SetPropertyNode>();
		case Node::Type::YAW_TO_DIR: return addNode<YawToDirNode>();
	}
	return nullptr;
}

LUMIX_STUDIO_ENTRY(visualscript)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), VisualScriptEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
