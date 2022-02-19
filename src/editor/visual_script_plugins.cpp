#define LUMIX_NO_CUSTOM_CRT
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/stream.h"
#include "engine/universe.h"

#include "imgui/imgui.h"


using namespace Lumix;

struct Link {
	u32 from;
	u32 to;
};

struct Variable {
	enum Type {
		NUMBER,

		COUNT
	};

	Variable(IAllocator& allocator) : name(allocator) {}

	String name;
	Type type;
};

struct Graph;

struct Node {
	void onNodeGUI(Graph& graph) {
		pin_counter_ = 0;
		ImGuiEx::BeginNode(id_, pos_);
		onGUI(graph);
		ImGuiEx::EndNode();
	}

	virtual void generate(OutputMemoryStream& blob) = 0;

	u32 id_ = 0;
	ImVec2 pos_ = ImVec2(0.f, 0.f);

protected:
	void inputPin() {
		ImGuiEx::Pin(id_ | (pin_counter_ << 16), true);
		++pin_counter_;
	}

	void outputPin() {
		ImGuiEx::Pin(id_ | (pin_counter_ << 16), false);
		++pin_counter_;
	}

	void flowInput() {
		ImGuiEx::Pin(id_ | (pin_counter_ << 16), true, ImGuiEx::PinShape::TRIANGLE);
		++pin_counter_;
	}

	void flowOutput() {
		ImGuiEx::Pin(id_ | (pin_counter_ << 16), false, ImGuiEx::PinShape::TRIANGLE);
		++pin_counter_;
	}

	virtual void onGUI(Graph& graph) = 0;
	u32 pin_counter_ = 0;
};


struct Graph {
	Graph(IAllocator& allocator)
		: allocator_(allocator)
		, nodes_(allocator)
		, links_(allocator)
		, variables_(allocator)
	{}

	IAllocator& allocator_;
	Array<UniquePtr<Node>> nodes_;
	Array<Link> links_;
	Array<Variable> variables_;

	u32 node_counter = 71;
	template <typename T, typename... Args>
	Node* addNode(Args&&... args) {
		UniquePtr<T> n = UniquePtr<T>::create(allocator_, static_cast<Args&&>(args)...);
		n->id_ = ++node_counter;
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
};

struct SequenceNode : Node {
	void onGUI(Graph& graph) override {
		flowInput(); ImGui::TextUnformatted(ICON_FA_LIST_OL);
		ImGui::SameLine();
		for (u32 i = 0; i < outputs_; ++i) {
			flowOutput();ImGui::NewLine();
		}
		if (ImGuiEx::IconButton(ICON_FA_PLUS, "Add")) ++outputs_;
	}
	
	void generate(OutputMemoryStream& blob) override {}

	u32 outputs_ = 2;
};

struct SelfNode : Node {
	void onGUI(Graph& graph) override {
		outputPin();
		ImGui::TextUnformatted("Self");
	}
	
	void generate(OutputMemoryStream& blob) override {
		blob << "this";
	}
};

struct SetYawNode : Node {
	void onGUI(Graph& graph) override {
		flowInput(); ImGui::TextUnformatted("Set Yaw");
		ImGui::SameLine();
		flowOutput(); ImGui::NewLine();
		inputPin(); ImGui::TextUnformatted("Entity");
		inputPin(); ImGui::TextUnformatted("Yaw");
	}

	void generate(OutputMemoryStream& blob) override {
	}
};

struct MouseMoveNode : Node {
	void onGUI(Graph& graph) override {
		flowOutput(); ImGui::TextUnformatted("Mouse move");
		outputPin(); ImGui::TextUnformatted("Delta X");
		outputPin(); ImGui::TextUnformatted("Delta Y");
	}

	void generate(OutputMemoryStream& blob) override {
		blob << "function onInputEvent(event)\n";
		
		blob << "end\n";
	}
};

struct UpdateNode : Node {
	void onGUI(Graph& graph) override {
		flowOutput();
		ImGui::TextUnformatted(ICON_FA_CLOCK "Update");
	}

	void generate(OutputMemoryStream& blob) override {
		blob << "function update(td)\n";
		//getOutput(0)->generate(blob);
		blob << "end\n";
	}
};

struct AddNode : Node {
	void generate(OutputMemoryStream& blob) override {}
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

	void generate(OutputMemoryStream& blob) override {}
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
	void generate(OutputMemoryStream& blob) override {}
	void onGUI(Graph& graph) override {
		outputPin();
		const char* var_name = var_ < (u32)graph.variables_.size() ? graph.variables_[var_].name.c_str() : "N/A";
		ImGui::Text(ICON_FA_PENCIL_ALT " %s", var_name);
	}

	u32 var_ = 0;
};

struct SetPropertyNode : Node {
	void generate(OutputMemoryStream& blob) override {
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


	void onWindowGUI() override {
		if (!is_open_) return;

		i32 hovered_node = -1;
		i32 hovered_link = -1;
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Visual script")) {

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
