#include "core/allocator.h"
#include "core/crt.h"
#include "core/log.h"
#include "core/math.h"
#include "core/os.h"
#include "core/profiler.h"
#include "core/stream.h"
#include "core/stack_array.h"
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/editor_asset.h"
#include "editor/property_grid.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/reflection.h"
#include "engine/world.h"
#include "../script.h"
#include "../m3_lumix.h"

#include "imgui/imgui.h"


using namespace Lumix;

namespace {

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
	IF = 0x04,
	ELSE = 0x05,
	END = 0x0B,
	CALL = 0x10,
	LOCAL_GET = 0x20,
	GLOBAL_GET = 0x23,
	GLOBAL_SET = 0x24,
	I32_CONST = 0x41,
	I64_CONST = 0x42,
	F32_CONST = 0x43,
	F64_CONST = 0x44,

	I32_EQ = 0x46,
	I32_NEQ = 0x47,
	I32_LT_S = 0x48,
	I32_GT_S = 0x4A,
	I32_LE_S = 0x4C,
	I32_GE_S = 0x4E,

	F32_EQ = 0x5B,
	F32_NEQ = 0x5C,
	F32_LT = 0x5D,
	F32_GT = 0x5E,
	F32_LE = 0x5F,
	F32_GE = 0x60,

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
	virtual ScriptValueType getOutputType(u32 idx, const Graph& graph) { return ScriptValueType::I32; }

	bool m_selected = false;
protected:
	struct NodeInput {
		Node* node;
		u32 input_idx;
		void generate(OutputMemoryStream& blob, const Graph& graph) { node->generate(blob, graph, input_idx); }
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
				if (import.ret_type == WASMType::VOID) {
					blob.write(u8(0)); // num results
				}
				else {
					blob.write(u8(1)); // num results
					blob.write(import.ret_type);
				}
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
	Graph(const Path& path, IAllocator& allocator)
		: m_allocator(allocator)
		, m_nodes(allocator)
		, m_links(allocator)
		, m_variables(allocator)
		, m_path(path)
	{}

	~Graph() {
		for (Node* n : m_nodes) {
			LUMIX_DELETE(m_allocator, n);
		}
	}

	bool load(const Path& path, FileSystem& fs) {
		OutputMemoryStream content(m_allocator);
		if (!fs.getContentSync(path, content)) {
			logError("Failed to read ", path);
			return false;
		}
		
		InputMemoryStream blob(content);
		if (!deserialize(blob)) {
			logError("Failed to deserialize ", path);
			return false;
		}
		return true;
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

	void clear() {
		for (Node* n : m_nodes) {
			LUMIX_DELETE(m_allocator, n);
		}
		m_nodes.clear();
		m_links.clear();
		m_variables.clear();
	}

	bool deserialize(InputMemoryStream& blob) {
		const u32 magic = blob.read<u32>();
		if (magic != MAGIC) return false;
		const u32 version = blob.read<u32>();
		if (version != 0) return false;
		
		blob.read(m_node_counter);
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
		blob.write(m_node_counter);
		
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

	template <typename T, typename... Args>
	Node* addNode(Args&&... args) {
		Node* n = LUMIX_NEW(m_allocator, T)(static_cast<Args&&>(args)...);
		n->m_id = ++m_node_counter;
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

	IAllocator& m_allocator;
	Array<Node*> m_nodes;
	Array<NodeEditorLink> m_links;
	Array<Variable> m_variables;
	Path m_path;

	u32 m_node_counter = 0;
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
		return ScriptValueType::I32;
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
		NodeOutput a = getInputNode(0, graph);
		NodeOutput b = getInputNode(1, graph);
		if (!a || !b) {
			m_error = "Missing input";
			return;
		}

		a.generate(blob, graph);
		b.generate(blob, graph);

		const ScriptValueType typeA = a.node->getOutputType(a.output_idx, graph);
		const ScriptValueType typeB = b.node->getOutputType(b.output_idx, graph);

		if (typeA != typeB) {
			m_error = "Types do not match";
			return;
		}

		if (typeA == ScriptValueType::FLOAT) {
			switch (T) {
				case Type::EQ: blob.write(WasmOp::F32_EQ); break;
				case Type::NEQ: blob.write(WasmOp::F32_NEQ); break;
				case Type::LT: blob.write(WasmOp::F32_LT); break;
				case Type::GT: blob.write(WasmOp::F32_GT); break;
				case Type::GTE: blob.write(WasmOp::F32_GE); break;
				case Type::LTE: blob.write(WasmOp::F32_LE); break;
				default: ASSERT(false); return;
			}
		}
		else {
			switch (T) {
				case Type::EQ: blob.write(WasmOp::I32_EQ); break;
				case Type::NEQ: blob.write(WasmOp::I32_NEQ); break;
				case Type::LT: blob.write(WasmOp::I32_LT_S); break;
				case Type::GT: blob.write(WasmOp::I32_GT_S); break;
				case Type::GTE: blob.write(WasmOp::I32_GE_S); break;
				case Type::LTE: blob.write(WasmOp::I32_LE_S); break;
				default: ASSERT(false); break;
			}
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
		
		cond.generate(blob, graph);
		blob.write(WasmOp::IF);
		blob.write(u8(0x40)); // block type
		true_branch.generate(blob, graph);
		blob.write(WasmOp::ELSE);
		false_branch.generate(blob, graph);
		blob.write(WasmOp::END);
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

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::FLOAT; }

	void serialize(OutputMemoryStream& blob) const override { blob.write(m_value); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_value); }

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

	void serialize(OutputMemoryStream& blob) const override { blob.write(m_is_on); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_is_on); }

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

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::I32; }

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

	ScriptValueType getOutputType(u32 idx, const Graph& graph) override { return ScriptValueType::FLOAT; }


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
		if (!n0) return ScriptValueType::I32;
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
		return ScriptValueType::I32;
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

	void serialize(OutputMemoryStream& blob) const override {
		blob.write(m_var);
	}

	void deserialize(InputMemoryStream& blob) override {
		blob.read(m_var);
	}

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

	void serialize(OutputMemoryStream& blob) const override {
		blob.write(m_var);
	}

	void deserialize(InputMemoryStream& blob) override {
		blob.read(m_var);
	}

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
		outputPin();
		ImGui::Text("%s.%s", reflection::getComponent(cmp_type)->name, prop);
		ImGui::EndGroup();
		
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

struct VisualScriptEditorWindow : AssetEditorWindow, NodeEditor {
	VisualScriptEditorWindow(const Path& path, struct VisualScriptEditor& editor, StudioApp& app, IAllocator& allocator) 
		: NodeEditor(allocator)
		, AssetEditorWindow(app)
		, m_app(app)
		, m_allocator(allocator)
		, m_editor(editor)
		, m_graph(path, m_allocator)
	{
		m_graph.load(path, app.getEngine().getFileSystem());
		pushUndo(NO_MERGE_UNDO);
		m_dirty = false;
	}

	void pushUndo(u32 tag) override {
		SimpleUndoRedo::pushUndo(tag);
		m_dirty = true;
	}

	void deleteSelectedNodes() {
		for (i32 i = m_graph.m_nodes.size() - 1; i >= 0; --i) {
			Node* node = m_graph.m_nodes[i];
			if (node->m_selected) {
				for (i32 j = m_graph.m_links.size() - 1; j >= 0; --j) {
					if (m_graph.m_links[j].getFromNode() == node->m_id || m_graph.m_links[j].getToNode() == node->m_id) {
						m_graph.m_links.erase(j);
					}
				}

				LUMIX_DELETE(m_graph.m_allocator, node);
				m_graph.m_nodes.swapAndPop(i);
			}
		}
		pushUndo(NO_MERGE_UNDO);
	}

	struct INodeTypeVisitor {
		struct ICreator {
			virtual Node* create(Graph& graph) = 0;
		};

		virtual ~INodeTypeVisitor() {}
		virtual bool beginCategory(const char* name) { return true; }
		virtual void endCategory() {}
		virtual INodeTypeVisitor& visit(const char* label, ICreator& creator, char shortcut = 0) = 0;
		INodeTypeVisitor& visit(const char* label, Node::Type type, char shortcut = 0) {
			struct : ICreator {
				Node* create(Graph& graph) override { return graph.createNode(type); }
				Node::Type type;
			} creator;
			creator.type = type;
			return visit(label, creator, shortcut);
		}
	};

	void visitTypes(INodeTypeVisitor& visitor) {
		if (visitor.beginCategory("Compare")) {
			visitor.visit("=", Node::Type::EQ)
			.visit("<>", Node::Type::NEQ)
			.visit("<", Node::Type::LT)
			.visit(">", Node::Type::GT)
			.visit("<=", Node::Type::LTE)
			.visit(">=", Node::Type::GTE)
			.endCategory();
		}
		
		if (visitor.beginCategory("Set variable")) {
			for (const Variable& var : m_graph.m_variables) {
				struct : INodeTypeVisitor::ICreator {
					Node* create(Graph& graph) override { return graph.addNode<SetVariableNode>(graph, idx); }
					u32 idx;
				} creator;
				creator.idx = u32(&var - m_graph.m_variables.begin());
				if (var.name.length() > 0) visitor.visit(var.name.c_str(), creator);
			}
			visitor.endCategory();
		}

		if (visitor.beginCategory("Get variable")) {
			for (const Variable& var : m_graph.m_variables) {
				struct : INodeTypeVisitor::ICreator {
					Node* create(Graph& graph) override {
						return graph.addNode<GetVariableNode>(graph, idx);
					}
					u32 idx;
				} creator;
				creator.idx = u32(&var - m_graph.m_variables.begin());
				if (var.name.length() > 0) visitor.visit(var.name.c_str(), creator);
			}
			visitor.endCategory();
		}

		if (visitor.beginCategory("Get property")) {
			for (const reflection::RegisteredComponent& cmp : reflection::getComponents()) {
				if (cmp.cmp->props.empty()) continue;

				if (visitor.beginCategory(cmp.cmp->name)) {
					struct : reflection::IEmptyPropertyVisitor {
						void visit(const reflection::Property<float>& prop) override {
							struct : INodeTypeVisitor::ICreator {
								Node* create(Graph& graph) override {
									return graph.addNode<GetPropertyNode>(cmp->cmp->component_type, prop->name, graph.m_allocator);
								}
								const reflection::RegisteredComponent* cmp;
								const reflection::Property<float>* prop;
							} creator;
							creator.cmp = cmp;
							creator.prop = &prop;
							type_visitor->visit(prop.name, creator);
						}
						const reflection::RegisteredComponent* cmp;
						INodeTypeVisitor* type_visitor;
					} prop_visitor;
					prop_visitor.type_visitor = &visitor;
					prop_visitor.cmp = &cmp;
					cmp.cmp->visit(prop_visitor);
					visitor.endCategory();
				}
			}	

			visitor.endCategory();
		}
		
		if (visitor.beginCategory("Set property")) {
			for (const reflection::RegisteredComponent& cmp : reflection::getComponents()) {
				if (cmp.cmp->props.empty()) continue;

				if (visitor.beginCategory(cmp.cmp->name)) {
					struct : reflection::IEmptyPropertyVisitor {
						void visit(const reflection::Property<float>& prop) override {
							struct : INodeTypeVisitor::ICreator {
								Node* create(Graph& graph) override {
									return graph.addNode<SetPropertyNode>(cmp->cmp->component_type, prop->name, graph.m_allocator);
								}
								const reflection::RegisteredComponent* cmp;
								const reflection::Property<float>* prop;
							} creator;
							creator.cmp = cmp;
							creator.prop = &prop;
							type_visitor->visit(prop.name, creator);
						}
						const reflection::RegisteredComponent* cmp;
						INodeTypeVisitor* type_visitor;
					} prop_visitor;
					prop_visitor.type_visitor = &visitor;
					prop_visitor.cmp = &cmp;
					cmp.cmp->visit(prop_visitor);
					visitor.endCategory();
				}
			}	

			visitor.endCategory();
		}

		if (visitor.beginCategory("Call")) {
			for (const reflection::RegisteredComponent& rcmp : reflection::getComponents()) {
				struct : INodeTypeVisitor::ICreator {
					Node* create(Graph& graph) override {
						return graph.addNode<CallNode>(cmp, f, graph.m_allocator);
					}
					reflection::ComponentBase* cmp;
					reflection::FunctionBase* f;
				} creator;
				creator.cmp = rcmp.cmp;
				if (!rcmp.cmp->functions.empty() && visitor.beginCategory(rcmp.cmp->name)) {
					for (reflection::FunctionBase* f : rcmp.cmp->functions) {
						creator.f = f;
						visitor.visit(f->name, creator);
					}
					visitor.endCategory();
				}
			}
			visitor.endCategory();
		}

		visitor.visit("Add", Node::Type::ADD, 'A')
			.visit("Constant", Node::Type::CONST, '1')
			.visit("If", Node::Type::IF, 'I')
			.visit("Key Input", Node::Type::KEY_INPUT)
			.visit("Mouse move", Node::Type::MOUSE_MOVE)
			.visit("Multiply", Node::Type::MUL, 'M')
			.visit("Self", Node::Type::SELF, 'S')
			.visit("Sequence", Node::Type::SEQUENCE)
			.visit("Set yaw", Node::Type::SET_YAW)
			.visit("Start", Node::Type::START)
			.visit("Switch", Node::Type::SWITCH)
			.visit("Update", Node::Type::UPDATE)
			.visit("Vector 3", Node::Type::VEC3, '3')
			.visit("Yaw to direction", Node::Type::YAW_TO_DIR);
	}

	void onCanvasClicked(ImVec2 pos, i32 hovered_link) override {
		struct : INodeTypeVisitor {
			INodeTypeVisitor& visit(const char* label, ICreator& creator, char shortcut = 0) override {
				if (shortcut && os::isKeyDown((os::Keycode)shortcut)) {
					n = creator.create(plugin->m_graph);
				}
				return *this;
			}
			VisualScriptEditorWindow* plugin;
			Node* n = nullptr;
		} visitor;
		visitor.plugin = this;
		visitTypes(visitor);
		if (visitor.n) {
			visitor.n->m_pos = pos;
			if (hovered_link >= 0) splitLink(m_graph.m_nodes.back(), m_graph.m_links, hovered_link);
			pushUndo(NO_MERGE_UNDO);
		}
	}
	
	void onLinkDoubleClicked(NodeEditorLink& link, ImVec2 pos) override {}
	
	void deserialize(InputMemoryStream& blob) override {
		m_graph.clear();
		m_graph.deserialize(blob);
	}

	void serialize(OutputMemoryStream& blob) override {
		m_graph.serialize(blob);
	}

	void saveAs(const Path& path) {
		OutputMemoryStream tmp(m_allocator);
		m_graph.generate(tmp); // to update errors
		OutputMemoryStream blob(m_allocator);
		m_graph.serialize(blob);
		FileSystem& fs = m_app.getEngine().getFileSystem();
		if (!fs.saveContentSync(path, blob)) {
			logError("Failed to save ", path);
		}
		else {
			m_graph.m_path = path;
			m_dirty = false;
		}
	}

	void menu() {
		CommonActions& actions = m_app.getCommonActions();

		if (m_app.checkShortcut(actions.del)) deleteSelectedNodes();
		else if (m_app.checkShortcut(actions.save)) saveAs(m_graph.m_path);
		else if (m_app.checkShortcut(actions.undo)) undo();
		else if (m_app.checkShortcut(actions.redo)) redo();

		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if (menuItem(actions.save, true)) saveAs(m_graph.m_path);
				if (ImGui::MenuItem("Save as")) m_show_save_as = true;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit")) {
				if (menuItem(actions.undo, canUndo())) undo();
				if (menuItem(actions.redo, canRedo())) redo();
				ImGui::EndMenu();
			}
			if (ImGuiEx::IconButton(ICON_FA_SAVE, "Save")) saveAs(m_graph.m_path);
			if (ImGuiEx::IconButton(ICON_FA_UNDO, "Undo", canUndo())) undo();
			if (ImGuiEx::IconButton(ICON_FA_REDO, "Redo", canRedo())) redo();
			ImGui::EndMenuBar();
		}

		FileSelector& fs = m_app.getFileSelector();
		if (fs.gui("Save As", &m_show_save_as, "lvs", true)) saveAs(Path(fs.getPath()));
	}

	const Path& getPath() override { return m_graph.m_path; }

	void windowGUI() override {
		menu();
		ImGui::Columns(2);
		static bool once = [](){ ImGui::SetColumnWidth(-1, 150); return true; }();
		for (Variable& var : m_graph.m_variables) {
			ImGui::PushID(&var);
			if (ImGuiEx::IconButton(ICON_FA_TRASH, "Delete")) {
				m_graph.m_variables.erase(u32(&var - m_graph.m_variables.begin()));
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
			m_graph.m_variables.emplace(m_allocator);
		}
			
		ImGui::NextColumn();
		static ImVec2 offset = ImVec2(0, 0);
		const ImVec2 editor_pos = ImGui::GetCursorScreenPos();
		nodeEditorGUI(m_graph.m_nodes, m_graph.m_links);
		ImGui::Columns();
	}

	bool propertyList(ComponentType& cmp_type, Span<char> property_name) {
		static char filter[32] = "";
		ImGui::SetNextItemWidth(150);
		ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
		for (const reflection::RegisteredComponent& cmp : reflection::getComponents()) {
			struct : reflection::IEmptyPropertyVisitor {
				void visit(const reflection::Property<float>& prop) override {
					StaticString<128> tmp(cmp_name, ".", prop.name);
					if ((!filter[0] || findInsensitive(tmp, filter)) && ImGui::Selectable(tmp)) {
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
		static char filter[64] = "";
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(150);
		ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter), ImGuiInputTextFlags_AutoSelectAll);

		if (filter[0]) {
			struct Visitor : INodeTypeVisitor {
				Visitor(IAllocator& allocator) : path(allocator) {}
				
				bool beginCategory(const char* name) override {
					path.emplace(name);
					return true;
				}

				void endCategory() override { path.pop(); }
				
				INodeTypeVisitor& visit(const char* label, ICreator& creator, char shortcut) override {
					if (created) return *this;
					if (findInsensitive(label, filter)) {
						StaticString<256> label_full;
						for (const auto& s : path) label_full.append(s, " / ");
						label_full.append(label);
						if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::Selectable(label_full)) {
							Node* n = creator.create(plugin->m_graph);
							n->m_pos = pos;
							plugin->pushUndo(NO_MERGE_UNDO);
							filter[0] = '\0';
							created = true;
						}
					}
					return *this;
				}
				ImVec2 pos;
				bool created = false;
				VisualScriptEditorWindow* plugin;
				StackArray<StaticString<64>, 2> path;
			} visitor(m_graph.m_allocator);
			visitor.pos = pos;
			visitor.plugin = this;
			visitTypes(visitor);
		}
		else {
			struct : INodeTypeVisitor {
				bool beginCategory(const char* name) override { return ImGui::BeginMenu(name); }
				void endCategory() override { return ImGui::EndMenu(); }
				INodeTypeVisitor& visit(const char* label, ICreator& creator, char shortcut) override {
					if (ImGui::Selectable(label)) {
						Node* n = creator.create(plugin->m_graph);
						n->m_pos = pos;
						plugin->pushUndo(NO_MERGE_UNDO);
					}
					return *this;
				}
				ImVec2 pos;
				VisualScriptEditorWindow* plugin;
			} visitor;
			visitor.pos = pos;
			visitor.plugin = this;
			visitTypes(visitor);
		}
	}

	const char* getName() const override { return "visualscript"; }

	IAllocator& m_allocator;
	StudioApp& m_app;
	VisualScriptEditor& m_editor;
	Graph m_graph;
	bool m_show_save_as = false;
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

struct VisualScriptEditor : StudioApp::IPlugin, PropertyGrid::IPlugin {
	VisualScriptEditor(StudioApp& app)
		: m_allocator(app.getAllocator(), "visual script editor")
		, m_app(app)
		, m_asset_plugin(*this)
	{
		AssetCompiler& compiler = app.getAssetCompiler();
		compiler.registerExtension("wasm", ScriptResource::TYPE);
		const char* exts[] = { "wasm" };
		compiler.addPlugin(m_asset_plugin, Span(exts));

		app.getPropertyGrid().addPlugin(*this);
	}

	~VisualScriptEditor() {
		m_app.getPropertyGrid().removePlugin(*this);
	}

	void onGUI(PropertyGrid& grid, Span<const EntityRef> entities, ComponentType cmp_type, const TextFilter& filter, WorldEditor& editor) override {
		if (filter.isActive()) return;
		if (cmp_type != SCRIPT_TYPE) return;
		if (entities.length() != 1) return;

		World* world = editor.getWorld();
		ScriptModule* module = (ScriptModule*)world->getModule(SCRIPT_TYPE);
		Script& script = module->getScript(entities[0]);
		
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

	void open(const Path& path) {
		UniquePtr<VisualScriptEditorWindow> new_win = UniquePtr<VisualScriptEditorWindow>::create(m_allocator, path, *this, m_app, m_allocator);
		m_app.getAssetBrowser().addWindow(new_win.move());
	}

	void init() override {}
	const char* getName() const override { return "visual_script_editor"; }
	bool showGizmo(struct WorldView& view, struct ComponentUID cmp) override { return false; }

	struct AssetPlugin : EditorAssetPlugin {
		AssetPlugin(VisualScriptEditor& editor)
			: EditorAssetPlugin("Visual script", "lvs", ScriptResource::TYPE, editor.m_app, editor.m_allocator)
			, m_editor(editor)
		{}

		void openEditor(const Path& path) override { m_editor.open(path); }

		bool compile(const Path& src) override {
			FileSystem& fs = m_editor.m_app.getEngine().getFileSystem();
			if (Path::hasExtension(src.c_str(), "wasm")) {
				ScriptResource::Header header;
				OutputMemoryStream compiled(m_editor.m_allocator);
				compiled.write(header);
				OutputMemoryStream wasm(m_editor.m_allocator);
				if (!fs.getContentSync(src, wasm)) {
					logError("Failed to read ", src);
					return false;
				}
				compiled.write(wasm.data(), wasm.size());
				return m_editor.m_app.getAssetCompiler().writeCompiledResource(src, Span(compiled.data(), (u32)compiled.size()));
			}
			else {
				Graph graph(Path(), m_editor.m_allocator);
			
				OutputMemoryStream blob(m_editor.m_allocator);
				if (!fs.getContentSync(src, blob)) {
					logError("Failed to read ", src);
					return false;
				}
				InputMemoryStream iblob(blob);
				if (!graph.deserialize(iblob)) {
					logError("Failed to deserialize ", src);
					return false;
				}

				OutputMemoryStream compiled(m_editor.m_allocator);
				graph.generate(compiled);
				return m_editor.m_app.getAssetCompiler().writeCompiledResource(src, Span(compiled.data(), (u32)compiled.size()));
			}
		}

		void createResource(OutputMemoryStream& blob) override {
			Graph graph(Path(), m_editor.m_allocator);
			graph.addNode<UpdateNode>(graph.m_allocator);
			graph.serialize(blob);
		}

		VisualScriptEditor& m_editor;
	};

	TagAllocator m_allocator;
	StudioApp& m_app;
	AssetPlugin m_asset_plugin;
};


LUMIX_STUDIO_ENTRY(visualscript) {
	PROFILE_FUNCTION();
	return LUMIX_NEW(app.getAllocator(), VisualScriptEditor)(app);
}

} // anonymous namespace