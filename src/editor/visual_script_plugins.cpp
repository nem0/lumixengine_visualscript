#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/property_grid.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/crt.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/log.h"
#include "engine/os.h"
#include "engine/reflection.h"
#include "engine/stream.h"
#include "engine/universe.h"
#include "../script.h"
#include "../../external/kvm.h"

#include "imgui/imgui.h"


using namespace Lumix;

static const u32 OUTPUT_FLAG = NodeEditor::OUTPUT_FLAG;
static const ComponentType SCRIPT_TYPE = reflection::getComponentType("script");

struct Variable {
	Variable(IAllocator& allocator) : name(allocator) {}
	String name;
	ScriptValueType type;
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
		START,
		IF,
		EQ,
		NEQ,
		GT,
		LT,
		GTE,
		LTE
	};

	bool nodeGUI() override {
		m_input_pin_counter = 0;
		m_output_pin_counter = 0;
		ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
		bool res = onGUI();
		if (m_error.length() > 0) {
			ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xff, 0, 0, 0xff));
		}
		ImGuiEx::EndNode();
		if (m_error.length() > 0) {
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_error.c_str());
		}
		return res;
	}
	
	void nodeTitle(const char* title, bool input_flow, bool output_flow) {
		ImGuiEx::BeginNodeTitleBar();
		if (input_flow) flowInput();
		if (output_flow) flowOutput();
		ImGui::TextUnformatted(title);
		ImGuiEx::EndNodeTitleBar();
	}

	void generateNext(kvm_bc_writer& writer, const Graph& graph) {
		NodeInput n = getOutputNode(0, graph);
		if (!n.node) return;
		n.node->generate(writer, graph, n.input_idx);
	}

	void clearError() { m_error = ""; }

	virtual Type getType() const = 0;

	virtual void generate(kvm_bc_writer& writer, const Graph& graph, u32 output_idx) = 0;
	virtual void serialize(OutputMemoryStream& blob) const {}
	virtual void deserialize(InputMemoryStream& blob) {}
	virtual ScriptValueType getOutputType(u32 idx, const Graph& graph) { return ScriptValueType::U32; }

	bool m_selected = false;
protected:
	struct NodeInput {
		Node* node;
		u32 input_idx;
	};

	NodeInput getOutputNode(u32 idx, const Graph& graph);
	Node(IAllocator& allocator) : m_error(allocator) {}

	struct NodeOutput {
		Node* node;
		u32 output_idx;
		operator bool() const { return node; }
		void generate(kvm_bc_writer& writer, const Graph& graph) {
			node->generate(writer, graph, output_idx);
		}
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
	String m_error;
};


struct Graph {
	Graph(IAllocator& allocator)
		: m_allocator(allocator)
		, m_nodes(allocator)
		, m_links(allocator)
		, m_variables(allocator)
	{}

	static constexpr u32 MAGIC = '_LVS';

	void generate(OutputMemoryStream& blob) {
		for (Node* node : m_nodes) {
			node->clearError();
		}
		ScriptResource::Header header;
		blob.write(header);
		kvm_u8 bytecode[4096];
		kvm_bc_writer writer;
		kvm_bc_start_write(&writer, bytecode, sizeof(bytecode));
		kvm_label update_label = kvm_bc_create_label(&writer);
		kvm_label start_label = kvm_bc_create_label(&writer);
		kvm_label mouse_move_label = kvm_bc_create_label(&writer);
		for (Node* node : m_nodes) {
			switch (node->getType()) {
				case Node::Type::MOUSE_MOVE:
					kvm_bc_place_label(&writer, mouse_move_label);
					node->generate(writer, *this, 0);
					break;
				case Node::Type::START:
					kvm_bc_place_label(&writer, start_label);
					node->generate(writer, *this, 0);
					break;
				case Node::Type::UPDATE:
					kvm_bc_place_label(&writer, update_label);
					node->generate(writer, *this, 0);
					break;
				default: break;
			}
		}
		kvm_bc_end(&writer);
		kvm_bc_end_write(&writer);
		
		blob.write(writer.labels[update_label]);
		blob.write(writer.labels[start_label]);
		blob.write(writer.labels[mouse_move_label]);
		blob.write(m_variables.size());
		for (const Variable& v : m_variables) {
			blob.writeString(v.name.c_str());
			blob.write(v.type);
		}
		const u32 bytecode_size = u32(writer.ip - bytecode);
		blob.write(bytecode_size);
		blob.write(bytecode, writer.ip - bytecode);
	}

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
			blob.read(var.type);
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
			blob.write(var.type);
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

template <auto T>
struct CompareNode : Node {
	CompareNode(IAllocator& allocator)
		: Node(allocator)
	{}
	
	Type getType() const override { return T; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override {
		NodeOutput n0 = getInputNode(0, graph);
		if (n0) return n0.node->getOutputType(n0.output_idx, graph);
		return ScriptValueType::U32;
	}

	bool onGUI() override {
		switch (T) {
			case Type::GT: nodeTitle(">", false, false); break;
			case Type::LT: nodeTitle("<", false, false); break;
			case Type::GTE: nodeTitle(">=", false, false); break;
			case Type::LTE: nodeTitle(">=", false, false); break;
			case Type::EQ: nodeTitle("=", false, false); break;
			case Type::NEQ: nodeTitle("<>", false, false); break;
			default: ASSERT(false); break;
		}
		outputPin();
		inputPin(); ImGui::TextUnformatted("A");
		inputPin(); ImGui::TextUnformatted("B");
		return false;
	}

	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput a = getInputNode(0, graph);
		NodeOutput b = getInputNode(1, graph);
		if (!a || !b) {
			m_error = "Missing input";
			return;
		}

		a.generate(writer, graph);
		b.generate(writer, graph);

		if (getOutputType(0, graph) == ScriptValueType::FLOAT) {
			switch (T) {
				case Type::EQ: kvm_bc_eq(&writer); return;
				case Type::NEQ: kvm_bc_neq(&writer); return;
				case Type::LT: kvm_bc_ltf(&writer); return;
				case Type::GT: kvm_bc_gtf(&writer); return;
				default: ASSERT(false); return;
			}
		}

		switch (T) {
			case Type::EQ: kvm_bc_eq(&writer); break;
			case Type::NEQ: kvm_bc_neq(&writer); break;
			case Type::LT: kvm_bc_lt(&writer); break;
			case Type::GT: kvm_bc_gt(&writer); break;
			default: ASSERT(false); break;
		}
	}
};

struct IfNode : Node {
	IfNode(IAllocator& allocator)
		: Node(allocator)
	{}
	
	Type getType() const override { return Type::IF; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle("If", false, false);
		ImGui::BeginGroup();
		flowInput(); ImGui::TextUnformatted(" ");
		inputPin(); ImGui::TextUnformatted("Condition");
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup();
		flowOutput(); ImGui::TextUnformatted("True");
		flowOutput(); ImGui::TextUnformatted("False");
		ImGui::EndGroup();
		return false;
	}

	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeInput true_branch = getOutputNode(0, graph);
		NodeInput false_branch = getOutputNode(1, graph);
		NodeOutput cond = getInputNode(1, graph);
		if (!true_branch.node || !false_branch.node) {
			m_error = "Missing outputs";
			return;
		}
		if (!cond) {
			m_error = "Missing condition";
			return;
		}
		
		kvm_u32 else_label = kvm_bc_create_label(&writer);
		kvm_u32 endif_label = kvm_bc_create_label(&writer);
		
		cond.generate(writer, graph);
		kvm_bc_jmp(&writer, else_label);
		true_branch.node->generate(writer, graph, 0);
		kvm_bc_jmp(&writer, endif_label);
		kvm_bc_place_label(&writer, else_label);
		false_branch.node->generate(writer, graph, 0);
		kvm_bc_place_label(&writer, endif_label);
	}
};

struct SequenceNode : Node {
	SequenceNode(Graph& graph)
		: Node(graph.m_allocator)
		, m_graph(graph) {}
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
	
	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		for (u32 i = 0; ; ++i) {
			NodeInput n = getOutputNode(i, graph);
			if (!n.node) return;
			n.node->generate(writer, graph, 0);
		}
	}
	Graph& m_graph;
};

struct SelfNode : Node {
	SelfNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::SELF; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }
	bool onGUI() override {
		outputPin();
		ImGui::TextUnformatted("Self");
		return false;
	}
	
	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::ENTITY; }

	void generate(kvm_bc_writer& writer, const Graph&, u32) override {
		kvm_bc_get(&writer, (kvm_i32)EnvironmentIndices::SELF);
	}
};

struct CallNode : Node {
	CallNode(IAllocator& allocator) : Node(allocator) {}
	CallNode(reflection::ComponentBase* component, reflection::FunctionBase* function, IAllocator& allocator)
		: Node(allocator)
		, component(component)
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
	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {}

	const reflection::ComponentBase* component = nullptr;
	const reflection::FunctionBase* function = nullptr;
};

struct SetYawNode : Node {
	SetYawNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::SET_YAW; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle("Set entity yaw", true, true);
		inputPin(); ImGui::TextUnformatted("Entity");
		inputPin(); ImGui::TextUnformatted("Yaw");
		return false;
	}

	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput o1 = getInputNode(1, graph);
		NodeOutput o2 = getInputNode(2, graph);
		if (!o1 || !o2) {
			m_error = "Missing inputs";
			return;
		}
		
		kvm_bc_const(&writer, (kvm_u32)ScriptSyscalls::SET_YAW);
		o1.generate(writer, graph);
		o2.generate(writer, graph);
		kvm_bc_syscall(&writer, 3);

		generateNext(writer, graph);
	}
};

struct ConstNode : Node {
	ConstNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::CONST; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }
	
	void serialize(OutputMemoryStream& blob) const { blob.write(m_value); }
	void deserialize(InputMemoryStream& blob) { blob.read(m_value); }

	bool onGUI() override {
		outputPin();
		return ImGui::DragFloat("##v", &m_value);
	}

	void generate(kvm_bc_writer& writer, const Graph& graph, u32 output_idx) override {
		kvm_bc_const_float(&writer, m_value);
	}

	float m_value = 0;
};

struct MouseMoveNode : Node {
	MouseMoveNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::MOUSE_MOVE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	ScriptValueType getOutputType(u32 idx, const Graph& graph) { return ScriptValueType::FLOAT; }


	bool onGUI() override {
		nodeTitle(ICON_FA_MOUSE " Mouse move", false, true);
		outputPin(); ImGui::TextUnformatted("Delta X");
		outputPin(); ImGui::TextUnformatted("Delta Y");
		return false;
	}
	
	
	void generate(kvm_bc_writer& writer, const Graph& graph, u32 output_idx) override {
		switch (output_idx) {
			case 0: {
				NodeInput o = getOutputNode(0, graph);
				if(o.node) o.node->generate(writer, graph, o.input_idx);
				kvm_bc_end(&writer);
				break;
			}
			case 1:
			case 2:
				kvm_bc_get_local(&writer, output_idx - 1);	
				break;
		}
	}
};

struct Vec3Node : Node {
	Vec3Node(IAllocator& allocator)
		: Node(allocator)
	{}
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
	void generate(kvm_bc_writer& writer, const Graph&, u32) override {}
};

struct YawToDirNode : Node {
	YawToDirNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::YAW_TO_DIR; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		inputPin(); ImGui::TextUnformatted("Yaw to dir");
		ImGui::SameLine();
		outputPin();
		return false;
	}

	void generate(kvm_bc_writer& writer, const Graph&, u32) override {}
};

struct StartNode : Node {
	StartNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::START; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle(ICON_FA_PLAY "Start", false, true);
		return false;
	}

	void generate(kvm_bc_writer& writer, const Graph& graph, u32 pin_idx) override {
		NodeInput o = getOutputNode(0, graph);
		if(o.node) o.node->generate(writer, graph, o.input_idx);
		kvm_bc_end(&writer);
	}
};

struct UpdateNode : Node {
	UpdateNode(IAllocator& allocator) 
		: Node(allocator)
	{}
	Type getType() const override { return Type::UPDATE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		nodeTitle(ICON_FA_CLOCK "Update", false, true);
		outputPin();
		ImGui::TextUnformatted("Time delta");
		return false;
	}
	
	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::FLOAT; }

	void generate(kvm_bc_writer& writer, const Graph& graph, u32 pin_idx) override {
		if (pin_idx == 0) {
			NodeInput o = getOutputNode(0, graph);
			if(o.node) o.node->generate(writer, graph, o.input_idx);
			kvm_bc_end(&writer);
		}
		else {
			kvm_bc_get_local(&writer, 0);	
		}
	}
};


struct MulNode : Node {
	MulNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::MUL; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0 || !n1) {
			m_error = "Missing inputs";
			return;
		}

		n0.generate(writer, graph);
		n1.generate(writer, graph);
		if (n0.node->getOutputType(n0.output_idx, graph) == ScriptValueType::FLOAT)
			kvm_bc_mulf(&writer);
		else
			kvm_bc_mul(&writer);
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
	AddNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::ADD; }

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override {
		NodeOutput n0 = getInputNode(0, graph);
		if (n0) return n0.node->getOutputType(n0.output_idx, graph);
		return ScriptValueType::U32;
	}


	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0 || !n1) {
			m_error = "Missing inputs";
			return;
		}

		n0.generate(writer, graph);
		n1.generate(writer, graph);
		if (n0.node->getOutputType(n0.output_idx, graph) == ScriptValueType::FLOAT)
			kvm_bc_addf(&writer);
		else
			kvm_bc_add(&writer);
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
	SetVariableNode(Graph& graph)
		: Node(graph.m_allocator)
		, m_graph(graph)
	{}
	SetVariableNode(Graph& graph, u32 var)
		: Node(graph.m_allocator)
		, m_graph(graph)
		, m_var(var)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	Type getType() const override { return Type::SET_VARIABLE; }

	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput n = getInputNode(1, graph);
		if (!n) {
			m_error = "Missing input";
			return;
		}
		n.generate(writer, graph);
		kvm_bc_set(&writer, ((u32)EnvironmentIndices::VARIABLES) + m_var);
		generateNext(writer, graph);
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
	GetVariableNode(Graph& graph)
		: Node(graph.m_allocator)
		, m_graph(graph)
	{}
	GetVariableNode(Graph& graph, u32 var)
		: Node(graph.m_allocator)
		, m_graph(graph)
		, m_var(var)
	{}
	Type getType() const override { return Type::GET_VARIABLE; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override {
		return graph.m_variables[m_var].type;
	}

	void generate(kvm_bc_writer& writer, const Graph&, u32) override {
		kvm_bc_get(&writer, ((u32)EnvironmentIndices::VARIABLES) + m_var);
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
	SetPropertyNode(IAllocator& allocator)
		: Node(allocator)
	{}	
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

	void serialize(OutputMemoryStream& blob) const override {
		blob.writeString(prop);
		blob.writeString(value);
		blob.writeString(reflection::getComponent(cmp_type)->name);
	}

	void deserialize(InputMemoryStream& blob) override {
		copyString(prop, blob.readString());
		copyString(value, blob.readString());
		cmp_type = reflection::getComponentType(blob.readString());
	}


	void generate(kvm_bc_writer& writer, const Graph& graph, u32) override {
		NodeOutput o = getInputNode(1, graph);
		if (!o) {
			m_error = "Missing inputs";
			return;
		}
		
		float v = (float)atof(value);

		kvm_bc_const(&writer, (kvm_u32)ScriptSyscalls::SET_PROPERTY);
		kvm_bc_const64(&writer, reflection::getPropertyHash(cmp_type, prop).getHashValue());
		kvm_bc_const(&writer, cmp_type.index);

		o.node->generate(writer, graph, o.output_idx);

		kvm_bc_const_float(&writer, v);
		kvm_bc_syscall(&writer, 6);

		generateNext(writer, graph);
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

struct VisualScriptPropertyGridPlugin : PropertyGrid::IPlugin {
	void onGUI(PropertyGrid& grid, Span<const EntityRef> entities, ComponentType cmp_type, WorldEditor& editor) override {
		if (cmp_type != SCRIPT_TYPE) return;
		if (entities.length() != 1) return;

		Universe* universe = editor.getUniverse();
		ScriptScene* scene = (ScriptScene*)universe->getScene(SCRIPT_TYPE);
		Script& script = scene->getScript(entities[0]);
		
		if (!script.m_resource) return;
		if (!script.m_resource->isReady()) return;
		if (script.m_environment.empty()) return;

		InputMemoryStream blob(script.m_environment);
		blob.skip((u32)EnvironmentIndices::VARIABLES * sizeof(u32));
		for (const ScriptResource::Variable& var : script.m_resource->m_variables) {
			switch(var.type) {
				case ScriptValueType::FLOAT: {
					float f = blob.read<float>();
					ImGui::LabelText(var.name.c_str(), "%f", f);
					break;
				}
				case ScriptValueType::I32:
				case ScriptValueType::U32: {
					u32 v = blob.read<u32>();
					ImGui::LabelText(var.name.c_str(), "%d", v);
					break;
				}
				case ScriptValueType::ENTITY: {
					blob.read<EntityPtr>();
					break;
				}
			}
		}
	}
};

struct VisualScriptAssetPlugin : AssetBrowser::Plugin, AssetCompiler::IPlugin {
	VisualScriptAssetPlugin(StudioApp& app)
		: AssetBrowser::Plugin(app.getAllocator())
		, m_app(app)
	{
	}

	void deserialize(InputMemoryStream& blob) override { ASSERT(false); }
	void serialize(OutputMemoryStream& blob) override {}

	bool onGUI(Span<struct Resource*> resource) override { return false; }
	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "visual_script"; }
	ResourceType getResourceType() const override { return ScriptResource::TYPE; }

	bool compile(const Path& src) override {
		Graph graph(m_app.getAllocator());
		FileSystem& fs = m_app.getEngine().getFileSystem();
		
		OutputMemoryStream blob(m_app.getAllocator());
		if (!fs.getContentSync(src, blob)) {
			logError("Failed to read ", src);
			return false;
		}
		InputMemoryStream iblob(blob);
		if (!graph.deserialize(iblob)) {
			logError("Failed to deserialize ", src);
			return false;
		}

		OutputMemoryStream compiled(m_app.getAllocator());
		graph.generate(compiled);
		return m_app.getAssetCompiler().writeCompiledResource(src.c_str(), Span(compiled.data(), (i32)compiled.size()));
	}

	StudioApp& m_app;
};

struct VisualScriptEditorPlugin : StudioApp::GUIPlugin, NodeEditor {
	VisualScriptEditorPlugin (StudioApp& app) 
		: NodeEditor(app.getAllocator())	
		, m_app(app)
		, m_recent_paths(app.getAllocator())
		, m_asset_plugin(app)
	{
		m_toggle_ui.init("Visual Script Editor", "Toggle visual script editor", "visualScriptEditor", "", true);
		m_toggle_ui.func.bind<&VisualScriptEditorPlugin::onToggleUI>(this);
		m_toggle_ui.is_selected.bind<&VisualScriptEditorPlugin::isOpen>(this);
		
		m_save_action.init(ICON_FA_SAVE "Save", "Visual script save", "visual_script_editor_save", ICON_FA_SAVE, os::Keycode::S, Action::Modifiers::CTRL, true);
		m_save_action.func.bind<&VisualScriptEditorPlugin::save>(this);
		m_save_action.plugin = this;
		
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
		app.addAction(&m_undo_action);
		app.addAction(&m_redo_action);
		app.addAction(&m_delete_action);
		app.addWindowAction(&m_toggle_ui);

		AssetCompiler& compiler = app.getAssetCompiler();
		compiler.registerExtension("lvs", ScriptResource::TYPE);
		const char* exts[] = { "lvs", nullptr };
		compiler.addPlugin(m_asset_plugin, exts);

		app.getAssetBrowser().addPlugin(m_asset_plugin);
		app.getPropertyGrid().addPlugin(m_property_grid_plugin);
		newGraph();
	}
	
	~VisualScriptEditorPlugin() {
		m_app.getAssetCompiler().removePlugin(m_asset_plugin);
		m_app.getAssetBrowser().removePlugin(m_asset_plugin);
		m_app.getPropertyGrid().removePlugin(m_property_grid_plugin);

		m_app.removeAction(&m_toggle_ui);
		m_app.removeAction(&m_save_action);
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
			{ 'E', "Equal", Node::Type::EQ },
			{ 'G', "Greater than", Node::Type::GT },
			{ 'I', "If", Node::Type::IF },
			{ 'L', "Less than", Node::Type::LT },
			{ 'M', "Multiply", Node::Type::MUL },
			{ 'N', "Not equal", Node::Type::NEQ},
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

	void save() {
		if (m_path.isEmpty()) m_show_save_as = true;
		else saveAs(m_path.c_str());
	}

	void saveAs(const char* path) {
		ASSERT(path[0]);
		OutputMemoryStream tmp(m_app.getAllocator());
		m_graph->generate(tmp); // to update errors
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
		m_graph->addNode<UpdateNode>(m_graph->m_allocator);
		pushUndo(NO_MERGE_UNDO);
	}

	void menu() {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("New")) newGraph();
				if (ImGui::MenuItem("Open")) m_show_open = true;
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
				ImGui::SetNextItemWidth(75);
				ImGui::Combo("##type", (i32*)&var.type, "u32\0i32\0float\0entity\0");
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

	void onContextMenu(ImVec2 pos) override {
		ImVec2 cp = ImGui::GetItemRectMin();
		IAllocator& allocator = m_graph->m_allocator;
		if (ImGui::BeginMenu("Add node")) {
			Node* n = nullptr;
			if (ImGui::Selectable("Add")) n = m_graph->addNode<AddNode>(allocator);
			if (ImGui::Selectable("Multiply")) n = m_graph->addNode<MulNode>(allocator);
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
			if (ImGui::BeginMenu("Compare")) {
				if (ImGui::Selectable("=")) n = m_graph->addNode<CompareNode<Node::Type::EQ>>(allocator);
				if (ImGui::Selectable("<>")) n = m_graph->addNode<CompareNode<Node::Type::NEQ>>(allocator);
				if (ImGui::Selectable("<")) n = m_graph->addNode<CompareNode<Node::Type::LT>>(allocator);
				if (ImGui::Selectable(">")) n = m_graph->addNode<CompareNode<Node::Type::GT>>(allocator);
				if (ImGui::Selectable("<=")) n = m_graph->addNode<CompareNode<Node::Type::GTE>>(allocator);
				if (ImGui::Selectable(">=")) n = m_graph->addNode<CompareNode<Node::Type::LTE>>(allocator);
				ImGui::EndMenu();
			}
			
			if (ImGui::Selectable("If")) n = m_graph->addNode<IfNode>(allocator);
			if (ImGui::Selectable("Self")) n = m_graph->addNode<SelfNode>(allocator);
			if (ImGui::Selectable("Set yaw")) n = m_graph->addNode<SetYawNode>(allocator);
			if (ImGui::Selectable("Mouse move")) n = m_graph->addNode<MouseMoveNode>(allocator);
			if (ImGui::Selectable("Constant")) n = m_graph->addNode<ConstNode>(allocator);
			if (ImGui::Selectable("Set property")) n = m_graph->addNode<SetPropertyNode>(allocator);
			if (ImGui::Selectable("Update")) n = m_graph->addNode<UpdateNode>(allocator);
			if (ImGui::Selectable("Vector 3")) n = m_graph->addNode<Vec3Node>(allocator);
			if (ImGui::Selectable("Yaw to direction")) n = m_graph->addNode<YawToDirNode>(allocator);
			if (ImGui::Selectable("Sequence")) n = m_graph->addNode<SequenceNode>(*m_graph);
			if (ImGui::Selectable("Start")) n = m_graph->addNode<StartNode>(allocator);
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
							m_graph->addNode<CallNode>(rcmp.cmp, f, allocator);
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
	Action m_undo_action;
	Action m_redo_action;
	Action m_delete_action;
	Array<String> m_recent_paths;
	bool m_show_save_as = false;
	bool m_show_open = false;
	bool m_has_focus = false;
	VisualScriptAssetPlugin m_asset_plugin;
	VisualScriptPropertyGridPlugin m_property_grid_plugin;
};

Node* Graph::createNode(Node::Type type) {
	switch (type) {
		case Node::Type::ADD: return addNode<AddNode>(m_allocator);
		case Node::Type::MUL: return addNode<MulNode>(m_allocator);
		case Node::Type::IF: return addNode<IfNode>(m_allocator);
		case Node::Type::EQ: return addNode<CompareNode<Node::Type::EQ>>(m_allocator);
		case Node::Type::NEQ: return addNode<CompareNode<Node::Type::NEQ>>(m_allocator);
		case Node::Type::LT: return addNode<CompareNode<Node::Type::LT>>(m_allocator);
		case Node::Type::GT: return addNode<CompareNode<Node::Type::GT>>(m_allocator);
		case Node::Type::LTE: return addNode<CompareNode<Node::Type::LTE>>(m_allocator);
		case Node::Type::GTE: return addNode<CompareNode<Node::Type::GTE>>(m_allocator);
		case Node::Type::SEQUENCE: return addNode<SequenceNode>(*this);
		case Node::Type::SELF: return addNode<SelfNode>(m_allocator);
		case Node::Type::SET_YAW: return addNode<SetYawNode>(m_allocator);
		case Node::Type::CONST: return addNode<ConstNode>(m_allocator);
		case Node::Type::MOUSE_MOVE: return addNode<MouseMoveNode>(m_allocator);
		case Node::Type::START: return addNode<StartNode>(m_allocator);
		case Node::Type::UPDATE: return addNode<UpdateNode>(m_allocator);
		case Node::Type::VEC3: return addNode<Vec3Node>(m_allocator);
		case Node::Type::CALL: return addNode<CallNode>(m_allocator);
		case Node::Type::GET_VARIABLE: return addNode<GetVariableNode>(*this);
		case Node::Type::SET_VARIABLE: return addNode<SetVariableNode>(*this);
		case Node::Type::SET_PROPERTY: return addNode<SetPropertyNode>(m_allocator);
		case Node::Type::YAW_TO_DIR: return addNode<YawToDirNode>(m_allocator);
	}
	return nullptr;
}

LUMIX_STUDIO_ENTRY(visualscript)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), VisualScriptEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
