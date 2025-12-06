// SPIR-V reflection tests for model shaders (vertex pulling architecture)
// These tests verify the model shader SPIR-V modules meet required contracts:
// - No vertex inputs (vertex pulling via storage buffer)
// - NonUniform decoration for texture array access (descriptor indexing)
// - Correct descriptor bindings for texture arrays and storage buffers
// - Push constant size within Vulkan 1.4 minimum guarantee
// - MRT output locations for deferred rendering

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// SPIR-V magic number
constexpr uint32_t SPIRV_MAGIC = 0x07230203;

// SPIR-V opcodes (from SPIR-V specification)
constexpr uint32_t OP_DECORATE = 71;
constexpr uint32_t OP_MEMBER_DECORATE = 72;
constexpr uint32_t OP_TYPE_POINTER = 32;
constexpr uint32_t OP_TYPE_STRUCT = 30;
constexpr uint32_t OP_TYPE_ARRAY = 28;
constexpr uint32_t OP_TYPE_RUNTIME_ARRAY = 29;
constexpr uint32_t OP_TYPE_INT = 21;
constexpr uint32_t OP_TYPE_FLOAT = 22;
constexpr uint32_t OP_TYPE_VECTOR = 23;
constexpr uint32_t OP_TYPE_MATRIX = 24;
constexpr uint32_t OP_TYPE_IMAGE = 25;
constexpr uint32_t OP_TYPE_SAMPLED_IMAGE = 27;
constexpr uint32_t OP_VARIABLE = 59;
constexpr uint32_t OP_CONSTANT = 43;
constexpr uint32_t OP_ACCESS_CHAIN = 65;
constexpr uint32_t OP_IN_BOUNDS_ACCESS_CHAIN = 66;

// SPIR-V decorations
constexpr uint32_t DECORATION_LOCATION = 30;
constexpr uint32_t DECORATION_BINDING = 33;
constexpr uint32_t DECORATION_DESCRIPTOR_SET = 34;
constexpr uint32_t DECORATION_NON_UNIFORM = 5300;
constexpr uint32_t DECORATION_BLOCK = 2;
constexpr uint32_t DECORATION_OFFSET = 35;

// SPIR-V storage classes
constexpr uint32_t STORAGE_CLASS_INPUT = 1;
constexpr uint32_t STORAGE_CLASS_OUTPUT = 3;
constexpr uint32_t STORAGE_CLASS_UNIFORM = 2;
constexpr uint32_t STORAGE_CLASS_UNIFORM_CONSTANT = 0;
constexpr uint32_t STORAGE_CLASS_PUSH_CONSTANT = 9;
constexpr uint32_t STORAGE_CLASS_STORAGE_BUFFER = 12;

// Path to compiled shaders (relative to test_data or build output)
std::filesystem::path getShaderPath(const std::string& shaderName)
{
	// Try multiple locations: build output, prebuilt, and relative to test data
	std::vector<std::filesystem::path> searchPaths = {
		std::filesystem::current_path() / "data" / "effects" / shaderName,
		std::filesystem::path(TEST_DATA_PATH) / ".." / ".." / "code" / "graphics" / "shaders" / "compiled" / shaderName,
		std::filesystem::path(TEST_DATA_PATH) / ".." / ".." / "build" / "generated_shaders" / shaderName,
	};

	for (const auto& path : searchPaths) {
		if (std::filesystem::exists(path)) {
			return path;
		}
	}

	// Return the expected path even if it doesn't exist (test will fail with informative message)
	return std::filesystem::path(TEST_DATA_PATH) / ".." / ".." / "code" / "graphics" / "shaders" / "compiled" / shaderName;
}

std::optional<std::vector<uint32_t>> loadSpirv(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		return std::nullopt;
	}

	auto size = file.tellg();
	if (size < 20 || size % 4 != 0) { // Minimum SPIR-V header + word alignment
		return std::nullopt;
	}

	file.seekg(0, std::ios::beg);
	std::vector<uint32_t> spirv(static_cast<size_t>(size) / 4);
	file.read(reinterpret_cast<char*>(spirv.data()), size);

	if (spirv.empty() || spirv[0] != SPIRV_MAGIC) {
		return std::nullopt;
	}

	return spirv;
}

// Extract opcode from instruction word
uint32_t getOpcode(uint32_t word)
{
	return word & 0xFFFF;
}

// Extract word count from instruction word
uint32_t getWordCount(uint32_t word)
{
	return word >> 16;
}

// Information about a variable in SPIR-V
struct VariableInfo {
	uint32_t id = 0;
	uint32_t typeId = 0;
	uint32_t storageClass = 0;
	std::optional<uint32_t> location;
	std::optional<uint32_t> binding;
	std::optional<uint32_t> descriptorSet;
};

// Information about a type in SPIR-V
struct TypeInfo {
	uint32_t id = 0;
	uint32_t opcode = 0;
	uint32_t pointedTypeId = 0; // For OpTypePointer
	uint32_t elementTypeId = 0; // For OpTypeArray/OpTypeRuntimeArray
	uint32_t storageClass = 0;  // For OpTypePointer
	bool isBlock = false;
	uint32_t memberCount = 0;   // For OpTypeStruct
	std::vector<uint32_t> memberTypeIds; // For OpTypeStruct
	uint32_t arrayLengthId = 0;          // For OpTypeArray (length is an IdRef)
	uint32_t bitWidth = 0;               // For OpTypeInt/OpTypeFloat
	uint32_t componentCount = 0;         // For OpTypeVector
	uint32_t columnCount = 0;            // For OpTypeMatrix
};

struct AccessChainInfo {
	uint32_t resultId = 0;
	uint32_t baseId = 0;
	std::vector<uint32_t> indices;
	uint32_t opcode = 0;
};

// Parsed SPIR-V module information
struct SpirvModuleInfo {
	std::vector<VariableInfo> variables;
	std::unordered_map<uint32_t, TypeInfo> types;
	std::unordered_set<uint32_t> nonUniformDecorations;
	std::unordered_map<uint32_t, uint32_t> constants; // id -> value
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> memberOffsets; // structId -> memberIndex -> offset
	std::vector<AccessChainInfo> accessChains;

	std::vector<VariableInfo> getInputVariables() const
	{
		std::vector<VariableInfo> inputs;
		for (const auto& var : variables) {
			if (var.storageClass == STORAGE_CLASS_INPUT) {
				inputs.push_back(var);
			}
		}
		return inputs;
	}

	std::vector<VariableInfo> getOutputVariables() const
	{
		std::vector<VariableInfo> outputs;
		for (const auto& var : variables) {
			if (var.storageClass == STORAGE_CLASS_OUTPUT) {
				outputs.push_back(var);
			}
		}
		return outputs;
	}

	std::vector<VariableInfo> getPushConstantVariables() const
	{
		std::vector<VariableInfo> pushConstants;
		for (const auto& var : variables) {
			if (var.storageClass == STORAGE_CLASS_PUSH_CONSTANT) {
				pushConstants.push_back(var);
			}
		}
		return pushConstants;
	}

	std::vector<VariableInfo> getDescriptorVariables() const
	{
		std::vector<VariableInfo> descriptors;
		for (const auto& var : variables) {
			if (var.descriptorSet.has_value() && var.binding.has_value()) {
				descriptors.push_back(var);
			}
		}
		return descriptors;
	}

	bool hasNonUniformDecoration() const
	{
		return !nonUniformDecorations.empty();
	}

	bool hasNonUniformDecoration(uint32_t id) const
	{
		return nonUniformDecorations.count(id) > 0;
	}

	// Check if a type is or contains an array type
	bool isArrayType(uint32_t typeId) const
	{
		auto it = types.find(typeId);
		if (it == types.end())
			return false;

		const auto& type = it->second;
		if (type.opcode == OP_TYPE_ARRAY || type.opcode == OP_TYPE_RUNTIME_ARRAY) {
			return true;
		}
		if (type.opcode == OP_TYPE_POINTER && type.pointedTypeId != 0) {
			return isArrayType(type.pointedTypeId);
		}
		return false;
	}

	// Check if a type is or contains a sampled image type
	bool isSampledImageType(uint32_t typeId) const
	{
		auto it = types.find(typeId);
		if (it == types.end())
			return false;

		const auto& type = it->second;
		if (type.opcode == OP_TYPE_SAMPLED_IMAGE) {
			return true;
		}
		if (type.opcode == OP_TYPE_POINTER && type.pointedTypeId != 0) {
			return isSampledImageType(type.pointedTypeId);
		}
		if ((type.opcode == OP_TYPE_ARRAY || type.opcode == OP_TYPE_RUNTIME_ARRAY) && type.elementTypeId != 0) {
			return isSampledImageType(type.elementTypeId);
		}
		return false;
	}

	// Check if a type is or contains a storage buffer type
	bool isStorageBufferType(uint32_t typeId) const
	{
		auto it = types.find(typeId);
		if (it == types.end())
			return false;

		const auto& type = it->second;
		if (type.opcode == OP_TYPE_POINTER && type.storageClass == STORAGE_CLASS_STORAGE_BUFFER) {
			return true;
		}
		return false;
	}

	std::optional<uint32_t> getTypeSize(uint32_t typeId) const;
};

SpirvModuleInfo parseSpirvModule(const std::vector<uint32_t>& spirv)
{
	SpirvModuleInfo info;

	// Skip header (5 words)
	size_t pos = 5;

	// Temporary storage for decorations until we process variables
	std::unordered_map<uint32_t, uint32_t> locationDecorations;
	std::unordered_map<uint32_t, uint32_t> bindingDecorations;
	std::unordered_map<uint32_t, uint32_t> setDecorations;
	std::unordered_set<uint32_t> blockDecorations;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> memberOffsets;

	// First pass: collect decorations and types
	while (pos < spirv.size()) {
		uint32_t opWord = spirv[pos];
		uint32_t opcode = getOpcode(opWord);
		uint32_t wordCount = getWordCount(opWord);

		if (wordCount == 0 || pos + wordCount > spirv.size()) {
			break;
		}

		switch (opcode) {
		case OP_DECORATE:
			if (wordCount >= 3) {
				uint32_t targetId = spirv[pos + 1];
				uint32_t decoration = spirv[pos + 2];

				if (decoration == DECORATION_LOCATION && wordCount >= 4) {
					locationDecorations[targetId] = spirv[pos + 3];
				} else if (decoration == DECORATION_BINDING && wordCount >= 4) {
					bindingDecorations[targetId] = spirv[pos + 3];
				} else if (decoration == DECORATION_DESCRIPTOR_SET && wordCount >= 4) {
					setDecorations[targetId] = spirv[pos + 3];
				} else if (decoration == DECORATION_NON_UNIFORM) {
					info.nonUniformDecorations.insert(targetId);
				} else if (decoration == DECORATION_BLOCK) {
					blockDecorations.insert(targetId);
				}
			}
			break;

		case OP_MEMBER_DECORATE:
			if (wordCount >= 5) {
				uint32_t targetId = spirv[pos + 1];
				uint32_t memberIndex = spirv[pos + 2];
				uint32_t decoration = spirv[pos + 3];
				if (decoration == DECORATION_OFFSET) {
					memberOffsets[targetId][memberIndex] = spirv[pos + 4];
				}
			}
			break;

		case OP_TYPE_INT:
			if (wordCount >= 4) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_INT;
				type.bitWidth = spirv[pos + 2];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_FLOAT:
			if (wordCount >= 3) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_FLOAT;
				type.bitWidth = spirv[pos + 2];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_VECTOR:
			if (wordCount >= 4) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_VECTOR;
				type.elementTypeId = spirv[pos + 2];
				type.componentCount = spirv[pos + 3];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_MATRIX:
			if (wordCount >= 4) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_MATRIX;
				type.elementTypeId = spirv[pos + 2]; // column type (vector)
				type.columnCount = spirv[pos + 3];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_POINTER:
			if (wordCount >= 4) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_POINTER;
				type.storageClass = spirv[pos + 2];
				type.pointedTypeId = spirv[pos + 3];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_STRUCT:
			if (wordCount >= 2) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_STRUCT;
				type.memberCount = wordCount - 2;
				type.memberTypeIds.reserve(type.memberCount);
				for (uint32_t idx = 0; idx < type.memberCount; ++idx) {
					type.memberTypeIds.push_back(spirv[pos + 2 + idx]);
				}
				type.isBlock = blockDecorations.count(type.id) > 0;
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_ARRAY:
			if (wordCount >= 4) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_ARRAY;
				type.elementTypeId = spirv[pos + 2];
				type.arrayLengthId = spirv[pos + 3];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_RUNTIME_ARRAY:
			if (wordCount >= 3) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_RUNTIME_ARRAY;
				type.elementTypeId = spirv[pos + 2];
				info.types[type.id] = type;
			}
			break;

		case OP_TYPE_SAMPLED_IMAGE:
			if (wordCount >= 3) {
				TypeInfo type;
				type.id = spirv[pos + 1];
				type.opcode = OP_TYPE_SAMPLED_IMAGE;
				info.types[type.id] = type;
			}
			break;

		case OP_CONSTANT:
			if (wordCount >= 4) {
				uint32_t id = spirv[pos + 2];
				uint32_t value = spirv[pos + 3];
				info.constants[id] = value;
			}
			break;

		case OP_VARIABLE:
			if (wordCount >= 4) {
				VariableInfo var;
				var.typeId = spirv[pos + 1];
				var.id = spirv[pos + 2];
				var.storageClass = spirv[pos + 3];

				if (locationDecorations.count(var.id)) {
					var.location = locationDecorations[var.id];
				}
				if (bindingDecorations.count(var.id)) {
					var.binding = bindingDecorations[var.id];
				}
				if (setDecorations.count(var.id)) {
					var.descriptorSet = setDecorations[var.id];
				}

				info.variables.push_back(var);
			}
			break;

		case OP_ACCESS_CHAIN:
		case OP_IN_BOUNDS_ACCESS_CHAIN:
			if (wordCount >= 4) {
				AccessChainInfo chain;
				chain.opcode = opcode;
				chain.resultId = spirv[pos + 2];
				chain.baseId = spirv[pos + 3];
				for (uint32_t idx = 4; idx < wordCount; ++idx) {
					chain.indices.push_back(spirv[pos + idx]);
				}
				info.accessChains.push_back(std::move(chain));
			}
			break;

		default:
			break;
		}

		pos += wordCount;
	}

	// Store member offsets collected from OpMemberDecorate
	info.memberOffsets = std::move(memberOffsets);

	return info;
}

std::optional<uint32_t> SpirvModuleInfo::getTypeSize(uint32_t typeId) const
{
	auto typeIt = types.find(typeId);
	if (typeIt == types.end()) {
		return std::nullopt;
	}

	const auto& type = typeIt->second;
	switch (type.opcode) {
	case OP_TYPE_INT:
	case OP_TYPE_FLOAT:
		if (type.bitWidth == 0 || type.bitWidth % 8 != 0) {
			return std::nullopt;
		}
		return type.bitWidth / 8;

	case OP_TYPE_VECTOR: {
		auto elemSize = getTypeSize(type.elementTypeId);
		if (!elemSize.has_value()) {
			return std::nullopt;
		}
		return elemSize.value() * type.componentCount;
	}

	case OP_TYPE_MATRIX: {
		// Matrix size is column count * size of each column vector
		auto columnSize = getTypeSize(type.elementTypeId);
		if (!columnSize.has_value()) {
			return std::nullopt;
		}
		return columnSize.value() * type.columnCount;
	}

	case OP_TYPE_ARRAY: {
		auto elemSize = getTypeSize(type.elementTypeId);
		if (!elemSize.has_value()) {
			return std::nullopt;
		}
		if (type.arrayLengthId == 0) {
			return std::nullopt;
		}
		auto lengthIt = constants.find(type.arrayLengthId);
		if (lengthIt == constants.end()) {
			return std::nullopt;
		}
		return elemSize.value() * lengthIt->second;
	}

	case OP_TYPE_RUNTIME_ARRAY:
		return std::nullopt; // Unknown length at compile time

	case OP_TYPE_POINTER:
		if (type.pointedTypeId == 0) {
			return std::nullopt;
		}
		return getTypeSize(type.pointedTypeId);

	case OP_TYPE_STRUCT: {
		if (type.memberTypeIds.size() != type.memberCount) {
			return std::nullopt;
		}
		auto offsetsIt = memberOffsets.find(type.id);
		if (offsetsIt == memberOffsets.end()) {
			return std::nullopt;
		}

		uint32_t structSize = 0;
		for (uint32_t memberIndex = 0; memberIndex < type.memberCount; ++memberIndex) {
			auto memberOffsetIt = offsetsIt->second.find(memberIndex);
			if (memberOffsetIt == offsetsIt->second.end()) {
				return std::nullopt; // Missing offset -> cannot compute
			}
			auto memberSize = getTypeSize(type.memberTypeIds[memberIndex]);
			if (!memberSize.has_value()) {
				return std::nullopt;
			}
			const uint32_t end = memberOffsetIt->second + memberSize.value();
			if (end > structSize) {
				structSize = end;
			}
		}
		return structSize;
	}

	default:
		return std::nullopt;
	}
}

class ModelShaderSpirvTest : public ::testing::Test {
  protected:
	void SetUp() override
	{
		m_vertShaderPath = getShaderPath("model.vert.spv");
		m_fragShaderPath = getShaderPath("model.frag.spv");
	}

	std::filesystem::path m_vertShaderPath;
	std::filesystem::path m_fragShaderPath;
};

} // namespace

// Test: Vertex shader has no vertex inputs (vertex pulling architecture)
// The model vertex shader should use storage buffer access instead of vertex attributes
TEST_F(ModelShaderSpirvTest, Scenario_ModelVertexShaderHasNoVertexInputs)
{
	// Load the vertex shader SPIR-V
	auto spirvOpt = loadSpirv(m_vertShaderPath);
	ASSERT_TRUE(spirvOpt.has_value())
		<< "Failed to load model.vert.spv from: " << m_vertShaderPath.string()
		<< "\nShader file does not exist or is invalid. Create the shader to make this test pass.";

	// Parse the module
	auto moduleInfo = parseSpirvModule(*spirvOpt);

	// Get input variables (StorageClass::Input with Location decoration)
	auto inputs = moduleInfo.getInputVariables();

	// Filter to only vertex inputs (those with Location decoration, excluding built-ins)
	std::vector<VariableInfo> vertexInputs;
	for (const auto& input : inputs) {
		if (input.location.has_value()) {
			vertexInputs.push_back(input);
		}
	}

	// Vertex pulling means zero vertex attribute inputs
	EXPECT_EQ(vertexInputs.size(), 0u)
		<< "Model vertex shader should have no vertex inputs (uses vertex pulling via storage buffer)."
		<< "\nFound " << vertexInputs.size() << " vertex input(s) with Location decoration.";
}

// Test: Fragment shader uses NonUniform decoration for texture array access
// Required for correct descriptor indexing with non-uniform indices
TEST_F(ModelShaderSpirvTest, Scenario_ModelFragmentShaderUsesNonUniformDecoration)
{
	// Load the fragment shader SPIR-V
	auto spirvOpt = loadSpirv(m_fragShaderPath);
	ASSERT_TRUE(spirvOpt.has_value())
		<< "Failed to load model.frag.spv from: " << m_fragShaderPath.string()
		<< "\nShader file does not exist or is invalid. Create the shader to make this test pass.";

	// Parse the module
	auto moduleInfo = parseSpirvModule(*spirvOpt);

	// The NonUniform decoration is required for correct descriptor indexing
	// when the index into the texture array is not uniform across invocations
	auto descriptors = moduleInfo.getDescriptorVariables();

	uint32_t textureArrayVarId = 0;
	for (const auto& desc : descriptors) {
		if (desc.descriptorSet.value_or(UINT32_MAX) == 0 && desc.binding.value_or(UINT32_MAX) == 1) {
			textureArrayVarId = desc.id;
			break;
		}
	}
	ASSERT_NE(textureArrayVarId, 0u)
		<< "Texture array descriptor (set=0, binding=1) not found in fragment shader.";

	std::vector<uint32_t> dynamicIndexIds;
	for (const auto& chain : moduleInfo.accessChains) {
		if (chain.baseId != textureArrayVarId) {
			continue;
		}
		for (uint32_t idxId : chain.indices) {
			// Treat indices that are not compile-time constants as dynamic
			if (moduleInfo.constants.count(idxId) == 0) {
				dynamicIndexIds.push_back(idxId);
			}
		}
	}

	ASSERT_FALSE(dynamicIndexIds.empty())
		<< "No dynamic index found for texture array access. Descriptor indexing requires non-uniform dynamic indexing.";

	bool hasNonUniformOnIndex = false;
	for (uint32_t idxId : dynamicIndexIds) {
		if (moduleInfo.hasNonUniformDecoration(idxId)) {
			hasNonUniformOnIndex = true;
			break;
		}
	}

	if (!hasNonUniformOnIndex) {
		// If other NonUniform decorations exist, report them as unrelated
		if (!moduleInfo.nonUniformDecorations.empty()) {
			std::stringstream ss;
			bool first = true;
			for (auto decoratedId : moduleInfo.nonUniformDecorations) {
				if (!first) {
					ss << ", ";
				}
				first = false;
				ss << decoratedId;
			}
			FAIL() << "NonUniform decoration is not applied to the dynamic texture array index.\n"
				   << "Dynamic index ids: " << dynamicIndexIds.front()
				   << " (and " << dynamicIndexIds.size() - 1 << " more if applicable)\n"
				   << "NonUniform is present on unrelated ids: [" << ss.str() << "].";
		} else {
			FAIL() << "Texture array dynamic index is not decorated NonUniform.";
		}
	}
}

// Test: Shader descriptor bindings match expected layout
// Set 0 binding 0: sampler2D array (combined image sampler)
// Set 0 binding 1: storage buffer (readonly vertex data)
TEST_F(ModelShaderSpirvTest, Scenario_ModelShaderDescriptorBindings)
{
	// Load both shaders
	auto vertSpirvOpt = loadSpirv(m_vertShaderPath);
	auto fragSpirvOpt = loadSpirv(m_fragShaderPath);

	ASSERT_TRUE(vertSpirvOpt.has_value())
		<< "Failed to load model.vert.spv from: " << m_vertShaderPath.string();
	ASSERT_TRUE(fragSpirvOpt.has_value())
		<< "Failed to load model.frag.spv from: " << m_fragShaderPath.string();

	auto vertModule = parseSpirvModule(*vertSpirvOpt);
	auto fragModule = parseSpirvModule(*fragSpirvOpt);

	// Combine descriptor variables from both stages
	auto vertDescriptors = vertModule.getDescriptorVariables();
	auto fragDescriptors = fragModule.getDescriptorVariables();

	// Look for set 0, binding 0 (storage buffer) in vertex shader
	bool foundStorageBuffer = false;
	for (const auto& desc : vertDescriptors) {
		if (desc.descriptorSet.value_or(UINT32_MAX) == 0 && desc.binding.value_or(UINT32_MAX) == 0) {
			// Should be a storage buffer type
			EXPECT_TRUE(vertModule.isStorageBufferType(desc.typeId))
				<< "Set 0 binding 0 should be a storage buffer (readonly vertex data)";
			foundStorageBuffer = true;
			break;
		}
	}
	EXPECT_TRUE(foundStorageBuffer)
		<< "Expected storage buffer at set 0, binding 0 in vertex shader";

	// Look for set 0, binding 1 (texture array) in fragment shader
	bool foundTextureArray = false;
	for (const auto& desc : fragDescriptors) {
		if (desc.descriptorSet.value_or(UINT32_MAX) == 0 && desc.binding.value_or(UINT32_MAX) == 1) {
			// Should be a sampled image array type
			EXPECT_TRUE(fragModule.isSampledImageType(desc.typeId))
				<< "Set 0 binding 1 should be a combined image sampler (sampler2D array)";
			EXPECT_TRUE(fragModule.isArrayType(desc.typeId))
				<< "Set 0 binding 1 should be an array type for texture array";
			foundTextureArray = true;
			break;
		}
	}
	EXPECT_TRUE(foundTextureArray)
		<< "Expected sampler2D array at set 0, binding 1 in fragment shader";
}

// Test: Push constant block size is within Vulkan 1.4 minimum guarantee (256 bytes)
TEST_F(ModelShaderSpirvTest, Scenario_ModelPushConstantBlockSize)
{
	// Load the vertex shader SPIR-V (push constants typically declared in vertex shader)
	auto spirvOpt = loadSpirv(m_vertShaderPath);
	ASSERT_TRUE(spirvOpt.has_value())
		<< "Failed to load model.vert.spv from: " << m_vertShaderPath.string()
		<< "\nShader file does not exist or is invalid. Create the shader to make this test pass.";

	auto moduleInfo = parseSpirvModule(*spirvOpt);

	// Find push constant variables
	auto pushConstants = moduleInfo.getPushConstantVariables();

	ASSERT_FALSE(pushConstants.empty())
		<< "Model shader must declare a push constant block describing vertex layout.";

	// For each push constant variable, verify the struct size
	constexpr uint32_t VULKAN_MIN_PUSH_CONSTANT_SIZE = 256;

	for (const auto& pc : pushConstants) {
		// Verify it's a pointer to a block type
		auto typeIt = moduleInfo.types.find(pc.typeId);
		ASSERT_TRUE(typeIt != moduleInfo.types.end())
			<< "Push constant type not found";
		EXPECT_EQ(typeIt->second.opcode, OP_TYPE_POINTER)
			<< "Push constant variable should have pointer type";
		EXPECT_EQ(typeIt->second.storageClass, STORAGE_CLASS_PUSH_CONSTANT)
			<< "Push constant pointer should have PushConstant storage class";

		uint32_t structTypeId = typeIt->second.pointedTypeId;
		ASSERT_NE(structTypeId, 0u) << "Push constant pointer must reference a struct type";

		auto sizeOpt = moduleInfo.getTypeSize(structTypeId);
		ASSERT_TRUE(sizeOpt.has_value())
			<< "Unable to compute push constant struct size (missing offsets or unsupported types)";

		EXPECT_LE(sizeOpt.value(), VULKAN_MIN_PUSH_CONSTANT_SIZE)
			<< "Push constant block exceeds 256 bytes ("
			<< sizeOpt.value() << " bytes). Vulkan 1.4 guarantees only 256 bytes.";
	}
}

// Test: Fragment shader declares 5 output locations (0-4) for deferred rendering MRT
TEST_F(ModelShaderSpirvTest, Scenario_ModelFragmentShaderOutputLocations)
{
	// Load the fragment shader SPIR-V
	auto spirvOpt = loadSpirv(m_fragShaderPath);
	ASSERT_TRUE(spirvOpt.has_value())
		<< "Failed to load model.frag.spv from: " << m_fragShaderPath.string()
		<< "\nShader file does not exist or is invalid. Create the shader to make this test pass.";

	auto moduleInfo = parseSpirvModule(*spirvOpt);

	// Get output variables
	auto outputs = moduleInfo.getOutputVariables();

	// Collect output locations (filter out built-ins which don't have Location decoration)
	std::set<uint32_t> outputLocations;
	for (const auto& output : outputs) {
		if (output.location.has_value()) {
			outputLocations.insert(output.location.value());
		}
	}

	// Expect 5 output locations: 0, 1, 2, 3, 4 for deferred rendering G-buffer
	// Typical layout:
	// location 0: albedo/diffuse
	// location 1: normal
	// location 2: position/depth
	// location 3: material properties (roughness, metallic, etc.)
	// location 4: emissive
	constexpr size_t EXPECTED_OUTPUT_COUNT = 5;

	EXPECT_EQ(outputLocations.size(), EXPECTED_OUTPUT_COUNT)
		<< "Fragment shader should declare exactly " << EXPECTED_OUTPUT_COUNT
		<< " output locations (0-4) for deferred rendering MRT."
		<< "\nFound " << outputLocations.size() << " output location(s).";

	// Verify specific locations 0-4 are present
	for (uint32_t loc = 0; loc < EXPECTED_OUTPUT_COUNT; ++loc) {
		EXPECT_TRUE(outputLocations.count(loc) > 0)
			<< "Missing output at location " << loc;
	}
}
