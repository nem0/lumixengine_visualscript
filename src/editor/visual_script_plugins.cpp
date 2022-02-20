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
#include "engine/stream.h"
#include "engine/universe.h"

#include "imgui/imgui.h"


using namespace Lumix;

struct Link {
	u32 from;
	u32 to;
};

static const u32 OUTPUT_FLAG = 1 << 15;

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

struct Node {
	void onNodeGUI(Graph& graph) {
		input_pin_counter_ = 0;
		output_pin_counter_ = 0;
		ImGuiEx::BeginNode(id_, pos_);
		onGUI(graph);
		ImGuiEx::EndNode();
	}

	virtual const char* getType() const = 0;

	virtual void generate(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) = 0;

	virtual void serialize(OutputMemoryStream& blob) {
		blob.write(id_);
		blob.write(pos_);
	}

	virtual void deserialize(InputMemoryStream& blob) {
		blob.read(id_);
		blob.read(pos_);
	}

	u32 id_ = 0;
	ImVec2 pos_ = ImVec2(0.f, 0.f);

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
		ImGuiEx::Pin(id_ | (input_pin_counter_ << 16), true);
		++input_pin_counter_;
	}

	void outputPin() {
		ImGuiEx::Pin(id_ | (output_pin_counter_ << 16) | OUTPUT_FLAG, false);
		++output_pin_counter_;
	}

	void flowInput() {
		ImGuiEx::Pin(id_ | (input_pin_counter_ << 16), true, ImGuiEx::PinShape::TRIANGLE);
		++input_pin_counter_;
	}

	void flowOutput() {
		ImGuiEx::Pin(id_ | (output_pin_counter_ << 16) | OUTPUT_FLAG, false, ImGuiEx::PinShape::TRIANGLE);
		++output_pin_counter_;
	}

	virtual void onGUI(Graph& graph) = 0;
	u32 input_pin_counter_ = 0;
	u32 output_pin_counter_ = 0;
};


struct Graph {
	Graph(IAllocator& allocator)
		: allocator_(allocator)
		, nodes_(allocator)
		, links_(allocator)
		, variables_(allocator)
	{}

	static constexpr u32 MAGIC = '_LVS';

	bool deserialize(InputMemoryStream& blob) {
		const u32 magic = blob.read<u32>();
		if (magic != MAGIC) return false;
		const u32 version = blob.read<u32>();
		if (version != 0) return false;
		
		blob.read(node_counter_);
		const u32 var_count = blob.read<u32>();
		variables_.reserve(var_count);
		for (u32 i = 0; i < var_count; ++i) {
			Variable& var = variables_.emplace(allocator_);
			var.name = blob.readString();
		}

		const u32 link_count = blob.read<u32>();
		links_.reserve(link_count);
		for (u32 i = 0; i < link_count; ++i) {
			Link& link = links_.emplace();
			blob.read(link);
		}

		const u32 node_count = blob.read<u32>();
		nodes_.reserve(node_count);
		for (u32 i = 0; i < node_count; ++i) {
			const char* type = blob.readString();
			UniquePtr<Node> n = createNode(type);
			n->deserialize(blob);
			nodes_.push(n.move());
		}
		return true;
	}

	UniquePtr<Node> createNode(const char* type) {
		
	}

	void serialize(OutputMemoryStream& blob) {
		blob.write(MAGIC);
		const u32 version = 0;
		blob.write(version);
		blob.write(node_counter_);
		
		blob.write(variables_.size());
		for (const Variable& var : variables_) {
			blob.writeString(var.name.c_str());
		}

		blob.write(links_.size());
		for (const Link& link : links_) {
			blob.write(link);
		}

		blob.write(nodes_.size());
		for (const UniquePtr<Node>& node : nodes_) {
			blob.writeString(node->getType());
			node->serialize(blob);
		}
	}

	IAllocator& allocator_;
	Array<UniquePtr<Node>> nodes_;
	Array<Link> links_;
	Array<Variable> variables_;

	u32 node_counter_ = 0;
	template <typename T, typename... Args>
	Node* addNode(Args&&... args) {
		UniquePtr<T> n = UniquePtr<T>::create(allocator_, static_cast<Args&&>(args)...);
		n->id_ = ++node_counter_;
		nodes_.push(n.move());
		return nodes_.last().get();
	}

	void removeNode(u32 node) {
		// TODO remove links
		nodes_.erase(node);
	}

	void removeLink(u32 link) {
		// TODO remove links
		links_.erase(link);
	}

	Node* getNode(u32 id) const {
		const i32 idx = nodes_.find([&](const UniquePtr<Node>& node){ return node->id_ == id; });
		return idx < 0 ? nullptr : nodes_[idx].get();
	}
};

Node::NodeInput Node::getOutputNode(u32 idx, const Graph& graph) {
	const i32 i = graph.links_.find([&](Link& l){
		return l.from == (id_ | (idx << 16) | OUTPUT_FLAG);
	});
	if (i == -1) return {nullptr, 0};

	const u32 to = graph.links_[i].to;
	return { graph.getNode(to & 0x7fFF), to >> 16 };
}

Node::NodeOutput Node::getInputNode(u32 idx, const Graph& graph) {
	const i32 i = graph.links_.find([&](Link& l){
		return l.to == (id_ | (idx << 16));
	});
	if (i == -1) return {nullptr, 0};

	const u32 from = graph.links_[i].from;
	return { graph.getNode(from & 0x7fFF), from >> 16 };
}

struct SequenceNode : Node {
	const char* getType() const override { return "sequence"; }
	void onGUI(Graph& graph) override {
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
	const char* getType() const override { return "self"; }
	void onGUI(Graph& graph) override {
		outputPin();
		ImGui::TextUnformatted("Self");
	}
	
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
		blob << "this";
	}
};

struct SetYawNode : Node {
	const char* getType() const override { return "set_yaw"; }
	void onGUI(Graph& graph) override {
		flowInput(); ImGui::TextUnformatted("Set Yaw");
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		inputPin(); ImGui::TextUnformatted("Entity");
		inputPin(); ImGui::TextUnformatted("Yaw");
	}

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
	}
};

struct ConstNode : Node {
	const char* getType() const override { return "const"; }
	void onGUI(Graph& graph) override {
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
	const char* getType() const override { return "mouse_move"; }
	void onGUI(Graph& graph) override {
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
			blob << "\t";
			blob << "\tend\n";
			blob << "end\n";
		}
	}
};

struct UpdateNode : Node {
	const char* getType() const override { return "update"; }
	void onGUI(Graph& graph) override {
		flowOutput();
		ImGui::TextUnformatted(ICON_FA_CLOCK "Update");
	}

	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
		blob << "function update(td)\n";
		//getOutput(0)->generate(blob);
		blob << "end\n";
	}
};

struct AddNode : Node {
	const char* getType() const override { return "add"; }

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		NodeOutput n0 = getInputNode(0, graph);
		NodeOutput n1 = getInputNode(1, graph);
		if (!n0.node || !n1.node) return;

		n0.node->generate(blob, graph, n0.output_idx);
		n1.node->generate(blob, graph, n1.output_idx);

		blob << "local v" << id_ << " = ";
		n0.node->printRef(blob, graph, n0.output_idx);
		blob << " + ";
		n1.node->printRef(blob, graph, n0.output_idx);
		blob << "\n";
	}

	void printRef(OutputMemoryStream& blob, const Graph& graph, u32 output_idx) override {
		blob << "v" << id_;
	}

	void onGUI(Graph& graph) override {
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
	SetVariableNode(u32 var) : var_(var) {}

	const char* getType() const override { return "set_variable"; }

	void generate(OutputMemoryStream& blob, const Graph& graph, u32) override {
		if (var_ >= (u32)graph.variables_.size()) return;
		
		const NodeOutput n = getInputNode(1, graph);
		if (!n.node) return;

		n.node->generate(blob, graph, n.output_idx);

		blob << graph.variables_[var_].name.c_str() << " = ";
		n.node->printRef(blob, graph, n.output_idx);
		blob << "\n";
	}

	void onGUI(Graph& graph) override {
		const char* var_name = var_ < (u32)graph.variables_.size() ? graph.variables_[var_].name.c_str() : "N/A";
		flowInput(); ImGui::Text("Set `%s`", var_name);
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		inputPin(); ImGui::TextUnformatted("Value");
	}

	u32 var_ = 0;
};

struct GetVariableNode : Node {
	GetVariableNode(u32 var) : var_(var) {}
	const char* getType() const override { return "get_variable"; }
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {}
	
	void printRef(OutputMemoryStream& blob, const Graph& graph, u32) override {
		if (var_ >= (u32)graph.variables_.size()) return;
		blob << graph.variables_[var_].name.c_str();
	}

	void onGUI(Graph& graph) override {
		outputPin();
		const char* var_name = var_ < (u32)graph.variables_.size() ? graph.variables_[var_].name.c_str() : "N/A";
		ImGui::Text(ICON_FA_PENCIL_ALT " %s", var_name);
	}

	u32 var_ = 0;
};

struct SetPropertyNode : Node {
	const char* getType() const override { return "set_property"; }
	void generate(OutputMemoryStream& blob, const Graph&, u32) override {
//		getInput(1)->generate(blob);
		blob << "." << cmp << "." << prop << " = " << value;
	}

	void onGUI(Graph& graph) override {
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

	void generate(const Path& path) {
		OutputMemoryStream blob(app_.getAllocator());
		for (Variable& var : graph_->variables_) {
			blob << "local " << var.name.c_str() << " = 0\n";
		}
		
		for (UniquePtr<Node>& node : graph_->nodes_) {
			if (equalStrings(node->getType(), "mouse_move")) {
				node->generate(blob, *graph_.get(), 0);
			}
		}
		
		os::OutputFile file;
		if (app_.getEngine().getFileSystem().open(path.c_str(), file)) {
			bool res = file.write(blob.data(), blob.size());
			if (!res) {
				logError("Could not write ", path);
			}
			file.close();
		}
	}

	void menu() {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				//if (ImGui::MenuItem("New")) newGraph();
				//if (ImGui::MenuItem("Load")) load();
				//if (ImGui::MenuItem("Load from entity", nullptr, false, emitter)) loadFromEntity();
				if (ImGui::MenuItem("Generate")) generate(Path("scripts/vs.lua"));
				//if (ImGui::MenuItem("Save as")) saveAs();
				//ImGui::Separator();
			
				//menuItem(m_apply_action, emitter && emitter->getResource());
				//ImGui::MenuItem("Autoapply", nullptr, &m_autoapply, emitter && emitter->getResource());

				ImGui::EndMenu();
			}
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
			for (Variable& var : graph_->variables_) {
				ImGui::PushID(&var);
				if (ImGuiEx::IconButton(ICON_FA_TRASH, "Delete")) {
					graph_->variables_.erase(u32(&var - graph_->variables_.begin()));
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
				graph_->variables_.emplace(app_.getAllocator());
			}
			
			ImGui::NextColumn();
			static ImVec2 offset = ImVec2(0, 0);
			const ImVec2 editor_pos = ImGui::GetCursorScreenPos();
			ImGuiEx::BeginNodeEditor("vs", &offset);
			
			for (UniquePtr<Node>& node : graph_->nodes_) {
				node->onNodeGUI(*graph_.get());
				if (ImGui::IsItemHovered()) {
					hovered_node = i32(&node - graph_->nodes_.begin());
				}
			}

			for (const Link& link : graph_->links_) {
				ImGuiEx::NodeLink(link.from, link.to);
				if (ImGuiEx::IsLinkHovered()) {
					hovered_link = i32(&link - graph_->links_.begin());
				}
			}

			ImGuiID link_from, link_to;
			if (ImGuiEx::GetNewLink(&link_from, &link_to)) {
				Link& link = graph_->links_.emplace();
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

			if (ImGui::BeginPopup("context_menu")) {
				ImVec2 cp = ImGui::GetItemRectMin();
				if (ImGui::BeginMenu("Add")) {
					Node* n = nullptr;
					if (ImGui::Selectable("Add")) n = graph_->addNode<AddNode>();
					if (ImGui::BeginMenu("Set variable")) {
						for (const Variable& var : graph_->variables_) {
							if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
								n = graph_->addNode<SetVariableNode>(u32(&var - graph_->variables_.begin()));
							}
						}
						ImGui::EndMenu();
					}
					if (ImGui::BeginMenu("Get variable")) {
						for (const Variable& var : graph_->variables_) {
							if (var.name.length() > 0 && ImGui::Selectable(var.name.c_str())) {
								n = graph_->addNode<GetVariableNode>(u32(&var - graph_->variables_.begin()));
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
					if (ImGui::Selectable("Sequence")) n = graph_->addNode<SequenceNode>();
					if (n) {
						n->pos_ = ImGui::GetMousePos() - editor_pos;
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
		}
		ImGui::End();
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


LUMIX_STUDIO_ENTRY(visualscript)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), VisualScriptEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
