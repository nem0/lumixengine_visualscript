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
#include "engine/world.h"
#include "../script.h"
#include "../m3_lumix.h"

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

enum class WASMLumixAPI : u32 {
	SET_YAW,
	SET_PROPERTY_FLOAT,
	GET_PROPERTY_FLOAT,

	COUNT
};

enum class WASMGlobals : u32 {
	SELF,

	USER
};

enum class WASMSection : u8 {
	TYPE = 1,
	IMPORT = 2,
	FUNCTION = 3,
	TABLE = 4,
	MEMORY = 5,
	GLOBAL = 6,
	EXPORT = 7,
	START = 8,
	ELEMENT = 9,
	CODE = 10,
	DATA = 11,
	DATA_COUNT = 12
};

enum class WASMExternalType : u8 {
	FUNCTION = 0,
	TABLE = 1,
	MEMORY = 2,
	GLOBAL = 3
};

enum class WASMType : u8 {
	F64 = 0x7C,
	F32 = 0x7D,
	I64 = 0x7E,
	I32 = 0x7F,

	VOID = 0xFF
};

enum class WasmOp : u8 {
	END = 0x0B,
	CALL = 0x10,
	LOCAL_GET = 0x20,
	GLOBAL_GET = 0x23,
	GLOBAL_SET = 0x24,
	I32_CONST = 0x41,
	I64_CONST = 0x42,
	F32_CONST = 0x43,
	F64_CONST = 0x44,
	I32_ADD = 0x6A,
	I32_MUL = 0x6C,
	F32_ADD = 0x92,
	F32_MUL = 0x94,
};

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
		LTE,
		KEY_INPUT,
		GET_PROPERTY,
		SWITCH
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

	void generateNext(OutputMemoryStream& blob, const Graph& graph) {
		NodeInput n = getOutputNode(0, graph);
		if (!n.node) return;
		n.node->generate(blob, graph, n.input_idx);
	}

	void clearError() { m_error = ""; }

	virtual Type getType() const = 0;

	virtual void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) = 0;
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
		void generate(OutputMemoryStream& blob, const Graph& graph) {
			node->generate(blob, graph, output_idx);
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

// TODO check if negative numbers are correctly handled
static void writeLEB128(OutputMemoryStream& blob, u64 val) {
  bool end;
  do {
	u8 byte = val & 0x7f;
	val >>= 7;
	end = ((val == 0 ) && ((byte & 0x40) == 0))
		|| ((val == -1) && ((byte & 0x40) != 0));
	if (!end) byte |= 0x80;
	blob.write(byte);
  } while (!end);
}

struct WASMWriter {
	using TypeHandle = u32;
	using FunctionHandle = u32;

	WASMWriter(IAllocator& allocator)
		: m_allocator(allocator)
		, m_exports(allocator)
		, m_imports(allocator)
		, m_globals(allocator)
	{}

	void addFunctionImport(const char* module_name, const char* field_name, WASMType ret_type, Span<const WASMType> args) {
		Import& import = m_imports.emplace(m_allocator);
		import.module_name = module_name;
		import.field_name = field_name;
		ASSERT(args.length() <= lengthOf(import.args));
		if (args.length() > 0) memcpy(import.args, args.begin(), args.length() * sizeof(args[0]));
		import.num_args = args.length();
		import.ret_type = ret_type;
	}

	void addFunctionExport(const char* name, Node* node, Span<const WASMType> args) {
		Export& e = m_exports.emplace(m_allocator);
		e.node = node;
		e.name = name;
		ASSERT(args.length() <= lengthOf(e.args));
		if (args.length() > 0) memcpy(e.args, args.begin(), args.length() * sizeof(args[0]));
		e.num_args = args.length();
	}
	
	void addGlobal(WASMType type, const char* export_name) {
		Global& global = m_globals.emplace(m_allocator);
		global.type = type;
		if (export_name) global.export_name = export_name;
	}

	void write(OutputMemoryStream& blob, Graph& graph) {
		blob.write(u32(0x6d736100));
		blob.write(u32(1));
	
		writeSection(blob, WASMSection::TYPE, [this](OutputMemoryStream& blob){
			writeLEB128(blob, m_imports.size() + m_exports.size());

			for (const Import& import : m_imports) {
				blob.write(u8(0x60)); // function
				blob.write(u8(import.num_args));
				for (u32 i = 0; i < import.num_args; ++i) {
					blob.write(import.args[i]);
				}
				blob.write(u8(import.ret_type == WASMType::VOID ? 0 : 1)); // num results
			}

			for (const Export& e : m_exports) {
				blob.write(u8(0x60)); // function
				blob.write(u8(e.num_args));
				for (u32 i = 0; i < e.num_args; ++i) {
					blob.write(e.args[i]);
				}
				blob.write(u8(0)); // num results
			}
		});

		writeSection(blob, WASMSection::IMPORT, [this](OutputMemoryStream& blob){
			writeLEB128(blob, m_imports.size());

			for (const Import& import : m_imports) {
				writeString(blob, import.module_name.c_str());
				writeString(blob, import.field_name.c_str());
				blob.write(WASMExternalType::FUNCTION);
				writeLEB128(blob, &import - m_imports.begin());
			}
		});

		writeSection(blob, WASMSection::FUNCTION, [this](OutputMemoryStream& blob){
			writeLEB128(blob, m_exports.size());
			
			for (const Export& func : m_exports) {
				writeLEB128(blob, m_imports.size() + (&func - m_exports.begin()));
			}
		});

		writeSection(blob, WASMSection::GLOBAL, [this](OutputMemoryStream& blob){
			writeLEB128(blob, m_globals.size());
			
			for (const Global& global : m_globals) {
				blob.write(global.type);
				blob.write(u8(1)); // mutable
				switch (global.type) {
					case WASMType::I32:
						blob.write(WasmOp::I32_CONST);
						blob.write(u8(0));
						break;
					case WASMType::I64:
						blob.write(WasmOp::I64_CONST);
						blob.write(u8(0));
						break;
					case WASMType::F32:
						blob.write(WasmOp::F32_CONST);
						blob.write(0.f);
						break;
					case WASMType::F64:
						blob.write(WasmOp::F64_CONST);
						blob.write(0.0);
						break;
					case WASMType::VOID:
						ASSERT(false);
						break;
				}
				blob.write(WasmOp::END);
			}
		});

		writeSection(blob, WASMSection::EXPORT, [this](OutputMemoryStream& blob){
			writeLEB128(blob, m_exports.size() + m_globals.size());

			for (const Export& e : m_exports) {
				writeString(blob, e.name.c_str());
				blob.write(WASMExternalType::FUNCTION);
				writeLEB128(blob, m_imports.size() + (&e - m_exports.begin()));
			}
			for (const Global& g : m_globals) {
				writeString(blob, g.export_name.c_str());
				blob.write(WASMExternalType::GLOBAL);
				writeLEB128(blob, &g - m_globals.begin());
			}
		});

		writeSection(blob, WASMSection::CODE, [this, &graph](OutputMemoryStream& blob){
			writeLEB128(blob, m_exports.size());
			OutputMemoryStream func_blob(m_allocator);
			
			for (const Export& code : m_exports) {
				func_blob.clear();
				code.node->generate(func_blob, graph, 0);
				writeLEB128(blob, (u32)func_blob.size());
				blob.write(func_blob.data(), func_blob.size());
			}
		});
	}
	
	static void writeString(OutputMemoryStream& blob, const char* value) {
		const i32 len = stringLength(value);
		writeLEB128(blob, len);
		blob.write(value, len);
	}

	template <typename F>
	void writeSection(OutputMemoryStream& blob, WASMSection section, F f) const {
		OutputMemoryStream tmp(m_allocator);
		f(tmp);
		blob.write(section);
		writeLEB128(blob, (u32)tmp.size());
		blob.write(tmp.data(), tmp.size());
	}

	struct Export {
		Export(IAllocator& allocator) : name(allocator) {}
		Node* node = nullptr;
		String name;
		u32 num_args = 0;
		WASMType args[8];
	};

	struct Global {
		Global(IAllocator& allocator) : export_name(allocator) {}
		String export_name;
		WASMType type;
	};

	struct Import {
		Import(IAllocator& allocator) : module_name(allocator), field_name(allocator) {}
		String module_name;
		String field_name;
		u32 num_args = 0;
		WASMType args[8];
		WASMType ret_type;
	};

	IAllocator& m_allocator;
	Array<Import> m_imports;
	Array<Global> m_globals;
	Array<Export> m_exports;
};

struct Graph {
	Graph(IAllocator& allocator)
		: m_allocator(allocator)
		, m_nodes(allocator)
		, m_links(allocator)
		, m_variables(allocator)
	{}

	~Graph() {
		for (Node* n : m_nodes) {
			LUMIX_DELETE(m_allocator, n);
		}
	}

	static constexpr u32 MAGIC = '_LVS';
	
	template <typename... Args>
	void addExport(WASMWriter& writer, Node::Type node_type, const char* name, Args... args) {
		for (Node* n : m_nodes) {
			if (n->getType() == node_type) {
				WASMType a[] = { args..., WASMType::VOID };
				writer.addFunctionExport(name, n, Span(a, lengthOf(a) - 1));
				break;
			}
		}
	}
	
	template <typename... Args>
	void addImport(WASMWriter& writer, const char* module_name, const char* field_name, WASMType ret_type, Args... args) {
		WASMType a[] = { args... };
		writer.addFunctionImport(module_name, field_name, ret_type, Span(a, lengthOf(a)));
	}

	void generate(OutputMemoryStream& blob) {
		for (Node* node : m_nodes) {
			node->clearError();
		}

		WASMWriter writer(m_allocator);
		addExport(writer, Node::Type::UPDATE, "update", WASMType::F32);
		addExport(writer, Node::Type::MOUSE_MOVE, "onMouseMove", WASMType::F32, WASMType::F32);
		addExport(writer, Node::Type::KEY_INPUT, "onKeyEvent", WASMType::I32);
		addExport(writer, Node::Type::START, "start");
		
		addImport(writer, "LumixAPI", "setYaw", WASMType::VOID, WASMType::I32, WASMType::F32);
		addImport(writer, "LumixAPI", "setPropertyFloat", WASMType::VOID, WASMType::I32, WASMType::I64, WASMType::F32);
		addImport(writer, "LumixAPI", "getPropertyFloat", WASMType::F32,  WASMType::I32, WASMType::I64);

		writer.addGlobal(WASMType::I32, "self");
		for (const Variable& var : m_variables) {
			switch (var.type) {
				case ScriptValueType::U32:
				case ScriptValueType::I32:
					writer.addGlobal(WASMType::I32, var.name.c_str());
					break;
				case ScriptValueType::FLOAT:
					writer.addGlobal(WASMType::F32, var.name.c_str());
					break;
				default: ASSERT(false); break;
			}
		}

		ScriptResource::Header header;
		blob.write(header);
		writer.write(blob, *this);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
#if 0 // TODO
		NodeOutput a = getInputNode(0, graph);
		NodeOutput b = getInputNode(1, graph);
		if (!a || !b) {
			m_error = "Missing input";
			return;
		}

		a.generate(blob, graph);
		b.generate(blob, graph);

		if (getOutputType(0, graph) == ScriptValueType::FLOAT) {
			switch (T) {
				case Type::EQ: kvm_bc_eq(&blob); return;
				case Type::NEQ: kvm_bc_neq(&blob); return;
				case Type::LT: kvm_bc_ltf(&blob); return;
				case Type::GT: kvm_bc_gtf(&blob); return;
				default: ASSERT(false); return;
			}
		}

		switch (T) {
			case Type::EQ: kvm_bc_eq(&blob); break;
			case Type::NEQ: kvm_bc_neq(&blob); break;
			case Type::LT: kvm_bc_lt(&blob); break;
			case Type::GT: kvm_bc_gt(&blob); break;
			default: ASSERT(false); break;
		}
#endif
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
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
		
#if 0 // TODO
		kvm_u32 else_label = kvm_bc_create_label(&blob);
		kvm_u32 endif_label = kvm_bc_create_label(&blob);
		
		cond.generate(blob, graph);
		kvm_bc_jmp(&blob, else_label);
		true_branch.node->generate(blob, graph, 0);
		kvm_bc_jmp(&blob, endif_label);
		kvm_bc_place_label(&blob, else_label);
		false_branch.node->generate(blob, graph, 0);
		kvm_bc_place_label(&blob, endif_label);
#endif
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
	
	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		for (u32 i = 0; ; ++i) {
			NodeInput n = getOutputNode(i, graph);
			if (!n.node) return;
			n.node->generate(blob, graph, 0);
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

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
		blob.write(WasmOp::GLOBAL_GET);
		writeLEB128(blob, (u32)WASMGlobals::SELF);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
#if 0 // TODO
		kvm_bc_const(&blob, (kvm_u32)ScriptSyscalls::CALL_CMP_METHOD);
		kvm_bc_const64(&blob, RuntimeHash(component->name).getHashValue());
		kvm_bc_const64(&blob, RuntimeHash(function->name).getHashValue());
		ASSERT(function->getArgCount() == 0);
		kvm_bc_syscall(&blob, 5);
#endif
	}

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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput o1 = getInputNode(1, graph);
		NodeOutput o2 = getInputNode(2, graph);
		if (!o1 || !o2) {
			m_error = "Missing inputs";
			return;
		}
		
		o1.generate(blob, graph);
		o2.generate(blob, graph);

		blob.write(WasmOp::CALL);
		writeLEB128(blob, (u32)WASMLumixAPI::SET_YAW);
		generateNext(blob, graph);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		blob.write(WasmOp::F32_CONST);
		blob.write(m_value);
	}

	float m_value = 0;
};

struct SwitchNode : Node {
	SwitchNode(IAllocator& allocator) : Node(allocator) {}
	Type getType() const override { return Type::SWITCH; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) const { blob.write(m_is_on); }
	void deserialize(InputMemoryStream& blob) { blob.read(m_is_on); }

	bool onGUI() override {
		nodeTitle("Switch", true, false);
		flowOutput(); ImGui::TextUnformatted("On");
		flowOutput(); ImGui::TextUnformatted("Off");
		return ImGui::Checkbox("Is On", &m_is_on);
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		if (m_is_on) {
			NodeInput n = getOutputNode(0, graph);
			if (!n.node) return;
			n.node->generate(blob, graph, n.input_idx);
		}
		else {
			NodeInput n = getOutputNode(1, graph);
			if (!n.node) return;
			n.node->generate(blob, graph, n.input_idx);
		}
	}

	bool m_is_on = true;
};

struct KeyInputNode : Node {
	KeyInputNode(IAllocator& allocator)
		: Node(allocator)
	{}
	Type getType() const override { return Type::KEY_INPUT; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	ScriptValueType getOutputType(u32 idx, const Graph& graph) { return ScriptValueType::U32; }

	bool onGUI() override {
		nodeTitle(ICON_FA_KEY " Key input", false, true);
		outputPin(); ImGui::TextUnformatted("Key");
		return false;
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		switch (output_idx) {
			case 0: {
				blob.write(u8(0)); // num locals
				NodeInput o = getOutputNode(0, graph);
				if(o.node) o.node->generate(blob, graph, o.input_idx);
				blob.write(WasmOp::END);
			}
			case 1:
				blob.write(WasmOp::LOCAL_GET);
				blob.write(u8(0));
				break;
			default:
				ASSERT(false);
				break;
		}
	}
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
	
	
	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		switch (output_idx) {
			case 0: {
				blob.write(u8(0)); // num locals
				NodeInput o = getOutputNode(0, graph);
				if(o.node) o.node->generate(blob, graph, o.input_idx);
				blob.write(WasmOp::END);
			}
			case 1:
				blob.write(WasmOp::LOCAL_GET);
				blob.write(u8(0));
				break;
			case 2:
				blob.write(WasmOp::LOCAL_GET);
				blob.write(u8(1));
				break;
			default:
				ASSERT(false);
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
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
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

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 pin_idx) override {
		blob.write(u8(0)); // num locals
		NodeInput o = getOutputNode(0, graph);
		if(o.node) o.node->generate(blob, graph, o.input_idx);
		blob.write(WasmOp::END);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 pin_idx) override {
		if (pin_idx == 0) {
			blob.write(u8(0)); // num locals
			NodeInput o = getOutputNode(0, graph);
			if(o.node) o.node->generate(blob, graph, o.input_idx);
			blob.write(WasmOp::END);
		}
		else {
			blob.write(WasmOp::LOCAL_GET);
			blob.write(u8(0));
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

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override{ 
		NodeOutput n0 = getInputNode(0, graph);
		if (!n0) return ScriptValueType::U32;
		return n0.node->getOutputType(n0.output_idx, graph);
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0 || !n1) {
			m_error = "Missing inputs";
			return;
		}

		n0.generate(blob, graph);
		n1.generate(blob, graph);
		if (n0.node->getOutputType(n0.output_idx, graph) == ScriptValueType::FLOAT)
			blob.write(WasmOp::F32_MUL);
		else
			blob.write(WasmOp::I32_MUL);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0 || !n1) {
			m_error = "Missing inputs";
			return;
		}

		n0.generate(blob, graph);
		n1.generate(blob, graph);
		if (n0.node->getOutputType(n0.output_idx, graph) == ScriptValueType::FLOAT)
			blob.write(WasmOp::F32_ADD);
		else
			blob.write(WasmOp::I32_ADD);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n = getInputNode(1, graph);
		if (!n) {
			m_error = "Missing input";
			return;
		}
		n.generate(blob, graph);
		blob.write(WasmOp::GLOBAL_SET);
		writeLEB128(blob, m_var + (u32)WASMGlobals::USER);
		generateNext(blob, graph);
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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		blob.write(WasmOp::GLOBAL_GET);
		writeLEB128(blob, m_var + (u32)WASMGlobals::USER);
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

struct GetPropertyNode : Node {
	GetPropertyNode(ComponentType cmp_type, const char* property_name, IAllocator& allocator)
		: Node(allocator)
		, cmp_type(cmp_type)
	{
		copyString(prop, property_name);
	}

	GetPropertyNode(IAllocator& allocator)
		: Node(allocator)
	{}

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::FLOAT; }

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }
	Type getType() const override { return Type::GET_PROPERTY; }

	void serialize(OutputMemoryStream& blob) const override {
		blob.writeString(prop);
		blob.writeString(reflection::getComponent(cmp_type)->name);
	}

	void deserialize(InputMemoryStream& blob) override {
		copyString(prop, blob.readString());
		cmp_type = reflection::getComponentType(blob.readString());
	}

	bool onGUI() override {
		nodeTitle("Get property", false, false);
		
		ImGui::BeginGroup();
		inputPin();
		ImGui::TextUnformatted("Entity");
		ImGui::Text("%s.%s", reflection::getComponent(cmp_type)->name, prop);
		ImGui::EndGroup();
		
		outputPin();
		return false;
	}

	void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		// TODO handle other types than float
		NodeOutput o = getInputNode(0, graph);
		if (!o) {
			m_error = "Missing entity input";
			return;
		}

		o.generate(blob, graph);
		
		const StableHash prop_hash = reflection::getPropertyHash(cmp_type, prop);
		blob.write(WasmOp::I64_CONST);
		writeLEB128(blob, prop_hash.getHashValue());

		blob.write(WasmOp::CALL);
		writeLEB128(blob, (u32)WASMLumixAPI::GET_PROPERTY_FLOAT);
	}

	char prop[64] = {};
	ComponentType cmp_type = INVALID_COMPONENT_TYPE;
};

struct SetPropertyNode : Node {
	SetPropertyNode(ComponentType cmp_type, const char* property_name, IAllocator& allocator)
		: Node(allocator)
		, cmp_type(cmp_type)
	{
		copyString(prop, property_name);
	}

	SetPropertyNode(IAllocator& allocator)
		: Node(allocator)
	{}

	Type getType() const override { return Type::SET_PROPERTY; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

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

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		// TODO handle other types than float
		NodeOutput o1 = getInputNode(1, graph);
		NodeOutput o2 = getInputNode(2, graph);
		if (!o1) {
			m_error = "Missing entity input";
			return;
		}

		o1.generate(blob, graph);
		
		const StableHash prop_hash = reflection::getPropertyHash(cmp_type, prop);
		blob.write(WasmOp::I64_CONST);
		writeLEB128(blob, prop_hash.getHashValue());

		if (o2.node) {
			o2.generate(blob, graph);
		}
		else {
			blob.write(WasmOp::F32_CONST);
			const float v = (float)atof(value);
			blob.write(v);
		}

		blob.write(WasmOp::CALL);
		writeLEB128(blob, (u32)WASMLumixAPI::SET_PROPERTY_FLOAT);
		generateNext(blob, graph);
	}

	bool onGUI() override {
		nodeTitle("Set property", true, true);
		
		inputPin();
		ImGui::TextUnformatted("Entity");
		ImGui::Text("%s.%s", reflection::getComponent(cmp_type)->name, prop);
		inputPin();
		ImGui::SetNextItemWidth(150);
		return ImGui::InputText("Value", value, sizeof(value));
	}
	
	char prop[64] = {};
	char value[64] = {};
	ComponentType cmp_type = INVALID_COMPONENT_TYPE;
};

struct VisualScriptPropertyGridPlugin : PropertyGrid::IPlugin {
	void onGUI(PropertyGrid& grid, Span<const EntityRef> entities, ComponentType cmp_type, WorldEditor& editor) override {
		if (cmp_type != SCRIPT_TYPE) return;
		if (entities.length() != 1) return;

		World* world = editor.getWorld();
		ScriptScene* scene = (ScriptScene*)world->getScene(SCRIPT_TYPE);
		Script& script = scene->getScript(entities[0]);
		
		if (!script.m_resource) return;
		if (!script.m_resource->isReady()) return;
		if (!script.m_module) return;

		for (i32 i = 0; i < m3l_getGlobalCount(script.m_module); ++i) {
			const char* name = m3l_getGlobalName(script.m_module, i);
			if (!name) continue;
			IM3Global global = m3_FindGlobal(script.m_module, name);
			M3TaggedValue val;
			m3_GetGlobal(global, &val);
			switch (val.type) {
				case M3ValueType::c_m3Type_none:
				case M3ValueType::c_m3Type_unknown:
				case M3ValueType::c_m3Type_i64:
				case M3ValueType::c_m3Type_f64:
					ASSERT(false); // TODO
					break;
				case M3ValueType::c_m3Type_i32:
					ImGui::LabelText(name, "%d", val.value.i32);
					break;
				case M3ValueType::c_m3Type_f32:
					ImGui::LabelText(name, "%f", val.value.f32);
					break;
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
		if (Path::hasExtension(src.c_str(), "wasm")) {
			ScriptResource::Header header;
			OutputMemoryStream compiled(m_app.getAllocator());
			compiled.write(header);
			FileSystem& fs = m_app.getEngine().getFileSystem();
			OutputMemoryStream wasm(m_app.getAllocator());
			if (!fs.getContentSync(src, wasm)) {
				logError("Failed to read ", src);
				return false;
			}
			compiled.write(wasm.data(), wasm.size());
			return m_app.getAssetCompiler().writeCompiledResource(src.c_str(), Span(compiled.data(), (i32)compiled.size()));
		}
		else {
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
	}

	StudioApp& m_app;
};

struct VisualScriptEditorPlugin : StudioApp::GUIPlugin, NodeEditor {
	VisualScriptEditorPlugin (StudioApp& app) 
		: NodeEditor(app.getAllocator())	
		, m_app(app)
		, m_recent_paths("visual_script_editor_recent_", 10, app)
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
		compiler.registerExtension("wasm", ScriptResource::TYPE);
		const char* exts[] = { "lvs", "wasm", nullptr };
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
		m_recent_paths.onSettingsLoaded();
	}

	void onBeforeSettingsSaved() override {
		Settings& settings = m_app.getSettings();
		settings.setValue(Settings::GLOBAL, "is_visualscript_editor_open", m_is_open);
		m_recent_paths.onBeforeSettingsSaved();
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
		m_recent_paths.push(m_path.c_str());
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
				if (const char* path = m_recent_paths.menu(); path) { load(path); }
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

	bool propertyList(ComponentType& cmp_type, Span<char> property_name) {
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
				char property_name[256];
			} visitor;
			visitor.filter = filter;
			visitor.cmp_name = cmp.cmp->name;
			cmp.cmp->visit(visitor);
			if (visitor.selected) {
				cmp_type = cmp.cmp->component_type;
				copyString(property_name, visitor.property_name);
				return true;
			}
		}	
		return false;
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
			if (ImGui::Selectable("Key Input")) n = m_graph->addNode<KeyInputNode>(allocator);
			if (ImGui::Selectable("Constant")) n = m_graph->addNode<ConstNode>(allocator);
			if (ImGui::BeginMenu("Get property")) {
				ComponentType cmp_type;
				char property_name[256];
				if (propertyList(cmp_type, Span(property_name))) {
					n = m_graph->addNode<GetPropertyNode>(cmp_type, property_name, allocator);
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Set property")) {
				ComponentType cmp_type;
				char property_name[256];
				if (propertyList(cmp_type, Span(property_name))) {
					n = m_graph->addNode<SetPropertyNode>(cmp_type, property_name, allocator);
				}
				ImGui::EndMenu();
			}
			if (ImGui::Selectable("Update")) n = m_graph->addNode<UpdateNode>(allocator);
			if (ImGui::Selectable("Vector 3")) n = m_graph->addNode<Vec3Node>(allocator);
			if (ImGui::Selectable("Yaw to direction")) n = m_graph->addNode<YawToDirNode>(allocator);
			if (ImGui::Selectable("Sequence")) n = m_graph->addNode<SequenceNode>(*m_graph);
			if (ImGui::Selectable("Start")) n = m_graph->addNode<StartNode>(allocator);
			if (ImGui::Selectable("Switch")) n = m_graph->addNode<SwitchNode>(allocator);
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
	RecentPaths m_recent_paths;
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
		case Node::Type::KEY_INPUT: return addNode<KeyInputNode>(m_allocator);
		case Node::Type::START: return addNode<StartNode>(m_allocator);
		case Node::Type::UPDATE: return addNode<UpdateNode>(m_allocator);
		case Node::Type::VEC3: return addNode<Vec3Node>(m_allocator);
		case Node::Type::CALL: return addNode<CallNode>(m_allocator);
		case Node::Type::GET_VARIABLE: return addNode<GetVariableNode>(*this);
		case Node::Type::SET_VARIABLE: return addNode<SetVariableNode>(*this);
		case Node::Type::SET_PROPERTY: return addNode<SetPropertyNode>(m_allocator);
		case Node::Type::YAW_TO_DIR: return addNode<YawToDirNode>(m_allocator);
		case Node::Type::GET_PROPERTY: return addNode<GetPropertyNode>(m_allocator);
		case Node::Type::SWITCH: return addNode<SwitchNode>(m_allocator);
	}
	return nullptr;
}

LUMIX_STUDIO_ENTRY(visualscript)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), VisualScriptEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
