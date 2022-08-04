// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Importer/GltfImporter.h>
#include <AnKi/Util/System.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Util/StringList.h>

#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wconversion"
#endif
#define CGLTF_IMPLEMENTATION
#include <Cgltf/cgltf.h>
#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic pop
#endif

namespace anki {

static F32 computeLightRadius(const Vec3 color)
{
	// Based on the attenuation equation: att = 1 - fragLightDist^2 / lightRadius^2
	const F32 minAtt = 0.01f;
	const F32 maxIntensity = max(max(color.x(), color.y()), color.z());
	return sqrt(maxIntensity / minAtt);
}

#if 0
static Error getUniformScale(const Mat4& m, F32& out)
{
	const F32 SCALE_THRESHOLD = 0.01f; // 1 cm

	Vec3 xAxis = m.getColumn(0).xyz();
	Vec3 yAxis = m.getColumn(1).xyz();
	Vec3 zAxis = m.getColumn(2).xyz();

	const F32 scale = xAxis.getLength();
	if(absolute(scale - yAxis.getLength()) > SCALE_THRESHOLD || absolute(scale - zAxis.getLength()) > SCALE_THRESHOLD)
	{
		ANKI_IMPORTER_LOGE("No uniform scale in the matrix");
		return Error::USER_DATA;
	}

	out = scale;
	return Error::NONE;
}
#endif

static void removeScale(Mat4& m)
{
	Vec3 xAxis = m.getColumn(0).xyz();
	Vec3 yAxis = m.getColumn(1).xyz();
	Vec3 zAxis = m.getColumn(2).xyz();

	xAxis.normalize();
	yAxis.normalize();
	zAxis.normalize();

	Mat3 rot;
	rot.setColumns(xAxis, yAxis, zAxis);
	m.setRotationPart(rot);
}

static void getNodeTransform(const cgltf_node& node, Vec3& tsl, Mat3& rot, Vec3& scale)
{
	if(node.has_matrix)
	{
		Mat4 trf = Mat4(node.matrix);

		Vec3 xAxis = trf.getColumn(0).xyz();
		Vec3 yAxis = trf.getColumn(1).xyz();
		Vec3 zAxis = trf.getColumn(2).xyz();

		scale = Vec3(xAxis.getLength(), yAxis.getLength(), zAxis.getLength());

		removeScale(trf);
		rot = trf.getRotationPart();
		tsl = trf.getTranslationPart().xyz();
	}
	else
	{
		if(node.has_translation)
		{
			tsl = Vec3(node.translation[0], node.translation[1], node.translation[2]);
		}
		else
		{
			tsl = Vec3(0.0f);
		}

		if(node.has_rotation)
		{
			rot = Mat3(Quat(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]));
		}
		else
		{
			rot = Mat3::getIdentity();
		}

		if(node.has_scale)
		{
			ANKI_ASSERT(node.scale[0] > 0.0f);
			ANKI_ASSERT(node.scale[1] > 0.0f);
			ANKI_ASSERT(node.scale[2] > 0.0f);
			scale = Vec3(node.scale[0], node.scale[1], node.scale[2]);
		}
		else
		{
			scale = Vec3(1.0f);
		}
	}
}

static Error getNodeTransform(const cgltf_node& node, Transform& trf)
{
	Vec3 tsl;
	Mat3 rot;
	Vec3 scale;
	getNodeTransform(node, tsl, rot, scale);

	const F32 scaleEpsilon = 0.01f;
	if(absolute(scale[0] - scale[1]) > scaleEpsilon || absolute(scale[0] - scale[2]) > scaleEpsilon)
	{
		ANKI_IMPORTER_LOGE("Expecting uniform scale");
		return Error::USER_DATA;
	}

	trf.setOrigin(tsl.xyz0());
	trf.setRotation(Mat3x4(Vec3(0.0f), rot));
	trf.setScale(scale[0]);

	return Error::NONE;
}

static Bool stringsExist(const HashMapAuto<CString, StringAuto>& map, const std::initializer_list<CString>& list)
{
	for(CString item : list)
	{
		if(map.find(item) != map.getEnd())
		{
			return true;
		}
	}

	return false;
}

const char* GltfImporter::XML_HEADER = R"(<?xml version="1.0" encoding="UTF-8" ?>)";

GltfImporter::GltfImporter(GenericMemoryPoolAllocator<U8> alloc)
	: m_alloc(alloc)
{
}

GltfImporter::~GltfImporter()
{
	if(m_gltf)
	{
		cgltf_free(m_gltf);
		m_gltf = nullptr;
	}

	m_alloc.deleteInstance(m_hive);
}

Error GltfImporter::init(const GltfImporterInitInfo& initInfo)
{
	m_inputFname.create(initInfo.m_inputFilename);
	m_outDir.create(initInfo.m_outDirectory);
	m_rpath.create(initInfo.m_rpath);
	m_texrpath.create(initInfo.m_texrpath);
	m_optimizeMeshes = initInfo.m_optimizeMeshes;
	m_comment.create(initInfo.m_comment);

	m_lightIntensityScale = max(initInfo.m_lightIntensityScale, EPSILON);

	m_lodCount = clamp(initInfo.m_lodCount, 1u, 3u);
	m_lodFactor = clamp(initInfo.m_lodFactor, 0.0f, 1.0f);
	if(m_lodFactor * F32(m_lodCount - 1) > 0.7f)
	{
		ANKI_IMPORTER_LOGE("LOD factor is too high %f", m_lodFactor);
		return Error::USER_DATA;
	}

	if(m_lodFactor < EPSILON || m_lodCount == 1)
	{
		m_lodCount = 1;
		m_lodFactor = 0.0f;
	}

	ANKI_IMPORTER_LOGV("Having %u LODs with LOD factor %f", m_lodCount, m_lodFactor);

	cgltf_options options = {};
	cgltf_result res = cgltf_parse_file(&options, m_inputFname.cstr(), &m_gltf);
	if(res != cgltf_result_success)
	{
		ANKI_IMPORTER_LOGE("Failed to open the GLTF file. Code: %d", res);
		return Error::FUNCTION_FAILED;
	}

	res = cgltf_load_buffers(&options, m_gltf, m_inputFname.cstr());
	if(res != cgltf_result_success)
	{
		ANKI_IMPORTER_LOGE("Failed to load GLTF data. Code: %d", res);
		return Error::FUNCTION_FAILED;
	}

	if(initInfo.m_threadCount > 0)
	{
		const U32 threadCount = min(getCpuCoresCount(), initInfo.m_threadCount);
		m_hive = m_alloc.newInstance<ThreadHive>(threadCount, m_alloc, true);
	}

	return Error::NONE;
}

Error GltfImporter::writeAll()
{
	populateNodePtrToIdx();

	for(const cgltf_animation* anim = m_gltf->animations; anim < m_gltf->animations + m_gltf->animations_count; ++anim)
	{
		ANKI_CHECK(writeAnimation(*anim));
	}

	StringAuto sceneFname(m_alloc);
	sceneFname.sprintf("%sScene.lua", m_outDir.cstr());
	ANKI_CHECK(m_sceneFile.open(sceneFname.toCString(), FileOpenFlag::WRITE));
	ANKI_CHECK(m_sceneFile.writeTextf("-- Generated by: %s\n", m_comment.cstr()));
	ANKI_CHECK(m_sceneFile.writeText("local scene = getSceneGraph()\nlocal events = getEventManager()\n"));

	// Nodes
	Error err = Error::NONE;
	for(const cgltf_scene* scene = m_gltf->scenes; scene < m_gltf->scenes + m_gltf->scenes_count && !err; ++scene)
	{
		for(cgltf_node* const* node = scene->nodes; node < scene->nodes + scene->nodes_count && !err; ++node)
		{
			err = visitNode(*(*node), Transform::getIdentity(), HashMapAuto<CString, StringAuto>(m_alloc));
		}
	}

	if(m_hive)
	{
		m_hive->waitAllTasks();
	}

	// Check error
	if(err)
	{
		ANKI_IMPORTER_LOGE("Error happened in main thread");
		return err;
	}

	const Error threadErr = m_errorInThread.load();
	if(threadErr)
	{
		ANKI_IMPORTER_LOGE("Error happened in a thread");
		return threadErr;
	}

	return err;
}

Error GltfImporter::getExtras(const cgltf_extras& extras, HashMapAuto<CString, StringAuto>& out)
{
	cgltf_size extrasSize;
	cgltf_copy_extras_json(m_gltf, &extras, nullptr, &extrasSize);
	if(extrasSize == 0)
	{
		return Error::NONE;
	}

	DynamicArrayAuto<char, PtrSize> json(m_alloc);
	json.create(extrasSize + 1);
	cgltf_result res = cgltf_copy_extras_json(m_gltf, &extras, &json[0], &extrasSize);
	if(res != cgltf_result_success)
	{
		ANKI_IMPORTER_LOGE("cgltf_copy_extras_json failed: %d", res);
		return Error::FUNCTION_FAILED;
	}

	json[json.getSize() - 1] = '\0';

	// Get token count
	CString jsonTxt(&json[0]);
	jsmn_parser parser;
	jsmn_init(&parser);
	const I tokenCount = jsmn_parse(&parser, jsonTxt.cstr(), jsonTxt.getLength(), nullptr, 0);
	if(tokenCount < 1)
	{
		return Error::NONE;
	}

	DynamicArrayAuto<jsmntok_t> tokens(m_alloc);
	tokens.create(U32(tokenCount));

	// Get tokens
	jsmn_init(&parser);
	jsmn_parse(&parser, jsonTxt.cstr(), jsonTxt.getLength(), &tokens[0], tokens.getSize());

	StringListAuto tokenStrings(m_alloc);
	for(const jsmntok_t& token : tokens)
	{
		if(token.type != JSMN_STRING && token.type != JSMN_PRIMITIVE)
		{
			continue;
		}

		StringAuto tokenStr(m_alloc);
		tokenStr.create(&jsonTxt[token.start], &jsonTxt[token.end]);
		tokenStrings.pushBack(tokenStr.toCString());
	}

	if((tokenStrings.getSize() % 2) != 0)
	{
		ANKI_IMPORTER_LOGE("Unable to parse: %s", jsonTxt.cstr());
		return Error::FUNCTION_FAILED;
	}

	// Write them to the map
	auto it = tokenStrings.getBegin();
	while(it != tokenStrings.getEnd())
	{
		auto it2 = it;
		++it2;

		out.emplace(it->toCString(), StringAuto(m_alloc, it2->toCString()));
		++it;
		++it;
	}

	return Error::NONE;
}

void GltfImporter::populateNodePtrToIdxInternal(const cgltf_node& node, U32& idx)
{
	m_nodePtrToIdx.emplace(&node, idx++);

	for(cgltf_node* const* c = node.children; c < node.children + node.children_count; ++c)
	{
		populateNodePtrToIdxInternal(**c, idx);
	}
}

void GltfImporter::populateNodePtrToIdx()
{
	U32 idx = 0;

	for(const cgltf_scene* scene = m_gltf->scenes; scene < m_gltf->scenes + m_gltf->scenes_count; ++scene)
	{
		for(cgltf_node* const* node = scene->nodes; node < scene->nodes + scene->nodes_count; ++node)
		{
			populateNodePtrToIdxInternal(**node, idx);
		}
	}
}

StringAuto GltfImporter::getNodeName(const cgltf_node& node)
{
	StringAuto out{m_alloc};

	if(node.name)
	{
		out.create(node.name);
	}
	else
	{
		auto it = m_nodePtrToIdx.find(&node);
		ANKI_ASSERT(it != m_nodePtrToIdx.getEnd());
		out.sprintf("unnamed_node_%u", *it);
	}

	return out;
}

Error GltfImporter::parseArrayOfNumbers(CString str, DynamicArrayAuto<F64>& out, const U32* expectedArraySize)
{
	StringListAuto list(m_alloc);
	list.splitString(str, ' ');

	out.create(U32(list.getSize()));

	Error err = Error::NONE;
	auto it = list.getBegin();
	auto end = list.getEnd();
	U32 i = 0;
	while(it != end && !err)
	{
		err = it->toNumber(out[i++]);
		++it;
	}

	if(err)
	{
		ANKI_IMPORTER_LOGE("Failed to parse floats: %s", str.cstr());
	}

	if(expectedArraySize && *expectedArraySize != out.getSize())
	{
		ANKI_IMPORTER_LOGE("Failed to parse floats. Expecting %u floats got %u: %s", *expectedArraySize, out.getSize(),
						   str.cstr());
	}

	return Error::NONE;
}

Error GltfImporter::visitNode(const cgltf_node& node, const Transform& parentTrf,
							  const HashMapAuto<CString, StringAuto>& parentExtras)
{
	// Check error from a thread
	const Error threadErr = m_errorInThread.load();
	if(threadErr)
	{
		ANKI_IMPORTER_LOGE("Error happened in a thread");
		return threadErr;
	}

	HashMapAuto<CString, StringAuto> outExtras(m_alloc);
	if(node.light)
	{
		ANKI_CHECK(writeLight(node, parentExtras));

		Transform localTrf;
		ANKI_CHECK(getNodeTransform(node, localTrf));
		localTrf.setScale(1.0f); // Remove scale
		ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
	}
	else if(node.camera)
	{
		ANKI_CHECK(writeCamera(node, parentExtras));

		Transform localTrf;
		ANKI_CHECK(getNodeTransform(node, localTrf));
		localTrf.setScale(1.0f); // Remove scale
		ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
	}
	else if(node.mesh)
	{
		// Handle special nodes
		HashMapAuto<CString, StringAuto> extras(parentExtras);
		ANKI_CHECK(getExtras(node.mesh->extras, extras));
		ANKI_CHECK(getExtras(node.extras, extras));

		HashMapAuto<CString, StringAuto>::Iterator it;

		const Bool skipRt = (it = extras.find("no_rt")) != extras.getEnd() && (*it == "true" || *it == "1");

		if((it = extras.find("particles")) != extras.getEnd())
		{
			const StringAuto& fname = *it;

			Bool gpuParticles = false;
			if((it = extras.find("gpu_particles")) != extras.getEnd() && *it == "true")
			{
				gpuParticles = true;
			}

			ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:new%sParticleEmitterNode(\"%s\")\n",
											  (gpuParticles) ? "Gpu" : "", getNodeName(node).cstr()));

			ANKI_CHECK(m_sceneFile.writeTextf("comp = node:getSceneNodeBase():get%sParticleEmitterComponent()\n",
											  (gpuParticles) ? "Gpu" : ""));
			ANKI_CHECK(m_sceneFile.writeTextf("comp:loadParticleEmitterResource(\"%s\")\n", fname.cstr()));

			Transform localTrf;
			ANKI_CHECK(getNodeTransform(node, localTrf));
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else if(stringsExist(extras, {"skybox_solid_color", "skybox_image", "fog_min_density", "fog_max_density",
									  "fog_height_of_min_density", "fog_height_of_max_density"}))
		{
			// Atmosphere

			ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:newSkyboxNode(\"%s\")\n", getNodeName(node).cstr()));
			ANKI_CHECK(m_sceneFile.writeText("comp = node:getSceneNodeBase():getSkyboxComponent()\n"));

			if((it = extras.find("skybox_solid_color")) != extras.getEnd())
			{
				StringListAuto tokens(m_alloc);
				tokens.splitString(*it, ' ');
				if(tokens.getSize() != 3)
				{
					ANKI_IMPORTER_LOGE("Error parsing \"skybox_solid_color\" of node %s", getNodeName(node).cstr());
					return Error::USER_DATA;
				}

				U count = 0;
				Vec3 solidColor(0.0f);
				for(auto& it : tokens)
				{
					F32 f;
					const Error err = it.toNumber(f);
					if(err)
					{
						ANKI_IMPORTER_LOGE("Error parsing \"skybox_solid_color\" of node %s", getNodeName(node).cstr());
						return Error::USER_DATA;
					}

					solidColor[count++] = f;
				}

				ANKI_CHECK(m_sceneFile.writeTextf("comp:setSolidColor(Vec3.new(%f, %f, %f))\n", solidColor.x(),
												  solidColor.y(), solidColor.z()));
			}
			else if((it = extras.find("skybox_image")) != extras.getEnd())
			{
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setImage(\"%s\")\n", it->cstr()));
			}

			if((it = extras.find("fog_min_density")) != extras.getEnd())
			{
				F32 val;
				ANKI_CHECK(it->toNumber(val));
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setMinFogDensity(\"%f\")\n", val));
			}

			if((it = extras.find("fog_max_density")) != extras.getEnd())
			{
				F32 val;
				ANKI_CHECK(it->toNumber(val));
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setMaxFogDensity(\"%f\")\n", val));
			}

			if((it = extras.find("fog_height_of_min_density")) != extras.getEnd())
			{
				F32 val;
				ANKI_CHECK(it->toNumber(val));
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setHeightOfMinFogDensity(\"%f\")\n", val));
			}

			if((it = extras.find("fog_height_of_max_density")) != extras.getEnd())
			{
				F32 val;
				ANKI_CHECK(it->toNumber(val));
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setHeightOfMaxFogDensity(\"%f\")\n", val));
			}

			Transform localTrf;
			ANKI_CHECK(getNodeTransform(node, localTrf));
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else if((it = extras.find("collision")) != extras.getEnd() && (*it == "true" || *it == "1"))
		{
			ANKI_CHECK(
				m_sceneFile.writeTextf("\nnode = scene:newStaticCollisionNode(\"%s\")\n", getNodeName(node).cstr()));

			ANKI_CHECK(m_sceneFile.writeText("comp = scene:getSceneNodeBase():getBodyComponent()\n"));
			const StringAuto meshFname = computeMeshResourceFilename(*node.mesh);
			ANKI_CHECK(m_sceneFile.writeTextf("comp:loadMeshResource(\"%s%s\")\n", m_rpath.cstr(), meshFname.cstr()));

			Transform localTrf;
			ANKI_CHECK(getNodeTransform(node, localTrf));
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else if((it = extras.find("reflection_probe")) != extras.getEnd() && (*it == "true" || *it == "1"))
		{
			Vec3 tsl;
			Mat3 rot;
			Vec3 scale;
			getNodeTransform(node, tsl, rot, scale);

			const Vec3 boxSize = scale * 2.0f;

			ANKI_CHECK(
				m_sceneFile.writeTextf("\nnode = scene:newReflectionProbeNode(\"%s\")\n", getNodeName(node).cstr()));

			ANKI_CHECK(m_sceneFile.writeText("comp = node:getSceneNodeBase():getReflectionProbeComponent()\n"));
			ANKI_CHECK(m_sceneFile.writeTextf("comp:setBoxVolumeSize(Vec3.new(%f, %f, %f))\n", boxSize.x(), boxSize.y(),
											  boxSize.z()));

			const Transform localTrf = Transform(tsl.xyz0(), Mat3x4(Vec3(0.0f), rot), 1.0f);
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else if((it = extras.find("gi_probe")) != extras.getEnd() && (*it == "true" || *it == "1"))
		{
			Vec3 tsl;
			Mat3 rot;
			Vec3 scale;
			getNodeTransform(node, tsl, rot, scale);

			const Vec3 boxSize = scale * 2.0f;

			F32 fadeDistance = -1.0f;
			if((it = extras.find("gi_probe_fade_distance")) != extras.getEnd())
			{
				ANKI_CHECK(it->toNumber(fadeDistance));
			}

			F32 cellSize = -1.0f;
			if((it = extras.find("gi_probe_cell_size")) != extras.getEnd())
			{
				ANKI_CHECK(it->toNumber(cellSize));
			}

			ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:newGlobalIlluminationProbeNode(\"%s\")\n",
											  getNodeName(node).cstr()));
			ANKI_CHECK(m_sceneFile.writeText("comp = node:getSceneNodeBase():getGlobalIlluminationProbeComponent()\n"));
			ANKI_CHECK(m_sceneFile.writeTextf("comp:setBoxVolumeSize(Vec3.new(%f, %f, %f))\n", boxSize.x(), boxSize.y(),
											  boxSize.z()));

			if(fadeDistance > 0.0f)
			{
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setFadeDistance(%f)\n", fadeDistance));
			}

			if(cellSize > 0.0f)
			{
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setCellSize(%f)\n", cellSize));
			}

			const Transform localTrf = Transform(tsl.xyz0(), Mat3x4(Vec3(0.0f), rot), 1.0f);
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else if((it = extras.find("decal")) != extras.getEnd() && (*it == "true" || *it == "1"))
		{
			StringAuto diffuseAtlas(m_alloc);
			if((it = extras.find("decal_diffuse_atlas")) != extras.getEnd())
			{
				diffuseAtlas.create(it->toCString());
			}

			StringAuto diffuseSubtexture(m_alloc);
			if((it = extras.find("decal_diffuse_sub_texture")) != extras.getEnd())
			{
				diffuseSubtexture.create(it->toCString());
			}

			F32 diffuseFactor = -1.0f;
			if((it = extras.find("decal_diffuse_factor")) != extras.getEnd())
			{
				ANKI_CHECK(it->toNumber(diffuseFactor));
			}

			StringAuto specularRougnessMetallicAtlas(m_alloc);
			if((it = extras.find("decal_specular_roughness_metallic_atlas")) != extras.getEnd())
			{
				specularRougnessMetallicAtlas.create(it->toCString());
			}

			StringAuto specularRougnessMetallicSubtexture(m_alloc);
			if((it = extras.find("decal_specular_roughness_metallic_sub_texture")) != extras.getEnd())
			{
				specularRougnessMetallicSubtexture.create(it->toCString());
			}

			F32 specularRougnessMetallicFactor = -1.0f;
			if((it = extras.find("decal_specular_roughness_metallic_factor")) != extras.getEnd())
			{
				ANKI_CHECK(it->toNumber(specularRougnessMetallicFactor));
			}

			ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:newDecalNode(\"%s\")\n", getNodeName(node).cstr()));
			ANKI_CHECK(m_sceneFile.writeText("comp = node:getSceneNodeBase():getDecalComponent()\n"));
			if(diffuseAtlas)
			{
				ANKI_CHECK(m_sceneFile.writeTextf("comp:setDiffuseDecal(\"%s\", \"%s\", %f)\n", diffuseAtlas.cstr(),
												  diffuseSubtexture.cstr(), diffuseFactor));
			}

			if(specularRougnessMetallicAtlas)
			{
				ANKI_CHECK(m_sceneFile.writeTextf(
					"comp:setSpecularRoughnessDecal(\"%s\", \"%s\", %f)\n", specularRougnessMetallicAtlas.cstr(),
					specularRougnessMetallicSubtexture.cstr(), specularRougnessMetallicFactor));
			}

			Vec3 tsl;
			Mat3 rot;
			Vec3 scale;
			getNodeTransform(node, tsl, rot, scale);
			const Transform localTrf = Transform(tsl.xyz0(), Mat3x4(Vec3(0.0f), rot), 1.0f);
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));
		}
		else
		{
			// Model node

			// Async because it's slow
			struct Ctx
			{
				GltfImporter* m_importer;
				const cgltf_mesh* m_mesh;
				Array<const cgltf_material*, 128> m_materials;
				U32 m_materialCount = 0;
				const cgltf_skin* m_skin;
				Bool m_rayTracing;
			};
			Ctx* ctx = m_alloc.newInstance<Ctx>();
			ctx->m_importer = this;
			ctx->m_mesh = node.mesh;
			for(U32 i = 0; i < node.mesh->primitives_count; ++i)
			{
				ctx->m_materials[ctx->m_materialCount++] = node.mesh->primitives[i].material;
			}
			ctx->m_skin = node.skin;
			ctx->m_rayTracing = !skipRt;

			HashMapAuto<CString, StringAuto>::Iterator it2;
			const Bool selfCollision = (it2 = extras.find("collision_mesh")) != extras.getEnd() && *it2 == "self";

			U32 maxLod = 0;
			if(m_lodCount > 1 && !skipMeshLod(*node.mesh, 1))
			{
				maxLod = 1;
			}
			if(m_lodCount > 2 && !skipMeshLod(*node.mesh, 2))
			{
				maxLod = 2;
			}

			// Thread task
			auto callback = [](void* userData, [[maybe_unused]] U32 threadId, [[maybe_unused]] ThreadHive& hive,
							   [[maybe_unused]] ThreadHiveSemaphore* signalSemaphore) {
				Ctx& self = *static_cast<Ctx*>(userData);

				// LOD 0
				Error err = self.m_importer->writeMesh(*self.m_mesh, 0, self.m_importer->computeLodFactor(0));

				// LOD 1
				if(!err && self.m_importer->m_lodCount > 1 && !self.m_importer->skipMeshLod(*self.m_mesh, 1))
				{
					err = self.m_importer->writeMesh(*self.m_mesh, 1, self.m_importer->computeLodFactor(1));
				}

				// LOD 2
				if(!err && self.m_importer->m_lodCount > 2 && !self.m_importer->skipMeshLod(*self.m_mesh, 2))
				{
					err = self.m_importer->writeMesh(*self.m_mesh, 2, self.m_importer->computeLodFactor(2));
				}

				for(U32 i = 0; i < self.m_materialCount && !err; ++i)
				{
					err = self.m_importer->writeMaterial(*self.m_materials[i], self.m_rayTracing);
				}

				if(!err)
				{
					err = self.m_importer->writeModel(*self.m_mesh);
				}

				if(!err && self.m_skin)
				{
					err = self.m_importer->writeSkeleton(*self.m_skin);
				}

				if(err)
				{
					self.m_importer->m_errorInThread.store(err._getCode());
				}

				self.m_importer->m_alloc.deleteInstance(&self);
			};

			if(m_hive)
			{
				m_hive->submitTask(callback, ctx);
			}
			else
			{
				callback(ctx, 0, *m_hive, nullptr);
			}

			ANKI_CHECK(writeModelNode(node, parentExtras));

			Transform localTrf;
			ANKI_CHECK(getNodeTransform(node, localTrf));
			ANKI_CHECK(writeTransform(parentTrf.combineTransformations(localTrf)));

			if(selfCollision)
			{
				ANKI_CHECK(m_sceneFile.writeTextf("node2 = scene:newStaticCollisionNode(\"%s_cl\")\n",
												  getNodeName(node).cstr()));

				ANKI_CHECK(m_sceneFile.writeText("comp = node2:getSceneNodeBase():getBodyComponent()\n"));

				const StringAuto meshFname = computeMeshResourceFilename(*node.mesh, maxLod);

				ANKI_CHECK(
					m_sceneFile.writeTextf("comp:loadMeshResource(\"%s%s\")\n", m_rpath.cstr(), meshFname.cstr()));
				ANKI_CHECK(m_sceneFile.writeText("comp:setWorldTransform(trf)\n"));
			}
		}
	}
	else
	{
		ANKI_IMPORTER_LOGV("Ignoring node %s. Assuming transform node", getNodeName(node).cstr());
		ANKI_CHECK(getExtras(node.extras, outExtras));
	}

	// Visit children
	Transform nodeTrf;
	{
		Vec3 tsl;
		Mat3 rot;
		Vec3 scale;
		getNodeTransform(node, tsl, rot, scale);
		nodeTrf = Transform(tsl.xyz0(), Mat3x4(Vec3(0.0f), rot), scale.x());
	}
	for(cgltf_node* const* c = node.children; c < node.children + node.children_count; ++c)
	{
		ANKI_CHECK(visitNode(*(*c), nodeTrf, outExtras));
	}

	return Error::NONE;
}

Error GltfImporter::writeTransform(const Transform& trf)
{
	ANKI_CHECK(m_sceneFile.writeText("trf = Transform.new()\n"));
	ANKI_CHECK(m_sceneFile.writeTextf("trf:setOrigin(Vec4.new(%f, %f, %f, 0))\n", trf.getOrigin().x(),
									  trf.getOrigin().y(), trf.getOrigin().z()));

	ANKI_CHECK(m_sceneFile.writeText("rot = Mat3x4.new()\n"));
	ANKI_CHECK(m_sceneFile.writeText("rot:setAll("));
	for(U i = 0; i < 12; i++)
	{
		ANKI_CHECK(m_sceneFile.writeTextf((i != 11) ? "%f, " : "%f)\n", trf.getRotation()[i]));
	}
	ANKI_CHECK(m_sceneFile.writeText("trf:setRotation(rot)\n"));

	ANKI_CHECK(m_sceneFile.writeTextf("trf:setScale(%f)\n", trf.getScale()));

	ANKI_CHECK(m_sceneFile.writeText("node:getSceneNodeBase():getMoveComponent():setLocalTransform(trf)\n"));

	return Error::NONE;
}

Error GltfImporter::writeModel(const cgltf_mesh& mesh)
{
	const StringAuto modelFname = computeModelResourceFilename(mesh);
	ANKI_IMPORTER_LOGV("Importing model %s", modelFname.cstr());

	HashMapAuto<CString, StringAuto> extras(m_alloc);
	ANKI_CHECK(getExtras(mesh.extras, extras));

	File file;
	StringAuto modelFullFname(m_alloc);
	modelFullFname.sprintf("%s/%s", m_outDir.cstr(), modelFname.cstr());
	ANKI_CHECK(file.open(modelFullFname, FileOpenFlag::WRITE));

	ANKI_CHECK(file.writeText("<model>\n"));
	ANKI_CHECK(file.writeText("\t<modelPatches>\n"));

	for(U32 primIdx = 0; primIdx < mesh.primitives_count; ++primIdx)
	{
		if(mesh.primitives_count == 1)
		{
			ANKI_CHECK(file.writeText("\t\t<modelPatch>\n"));
		}
		else
		{
			ANKI_CHECK(file.writeTextf("\t\t<modelPatch subMeshIndex=\"%u\">\n", primIdx));
		}

		{
			const StringAuto meshFname = computeMeshResourceFilename(mesh);
			ANKI_CHECK(file.writeTextf("\t\t\t<mesh>%s%s</mesh>\n", m_rpath.cstr(), meshFname.cstr()));
		}

		if(m_lodCount > 1 && !skipMeshLod(mesh, 1))
		{
			const StringAuto meshFname = computeMeshResourceFilename(mesh, 1);
			ANKI_CHECK(file.writeTextf("\t\t\t<mesh1>%s%s</mesh1>\n", m_rpath.cstr(), meshFname.cstr()));
		}

		if(m_lodCount > 2 && !skipMeshLod(mesh, 2))
		{
			const StringAuto meshFname = computeMeshResourceFilename(mesh, 2);
			ANKI_CHECK(file.writeTextf("\t\t\t<mesh2>%s%s</mesh2>\n", m_rpath.cstr(), meshFname.cstr()));
		}

		HashMapAuto<CString, StringAuto> materialExtras(m_alloc);
		ANKI_CHECK(getExtras(mesh.primitives[primIdx].material->extras, materialExtras));
		auto mtlOverride = materialExtras.find("material_override");
		if(mtlOverride != materialExtras.getEnd())
		{
			ANKI_CHECK(file.writeTextf("\t\t\t<material>%s</material>\n", mtlOverride->cstr()));
		}
		else
		{
			const StringAuto mtlFname = computeMaterialResourceFilename(*mesh.primitives[primIdx].material);
			ANKI_CHECK(file.writeTextf("\t\t\t<material>%s%s</material>\n", m_rpath.cstr(), mtlFname.cstr()));
		}

		ANKI_CHECK(file.writeText("\t\t</modelPatch>\n"));
	}

	ANKI_CHECK(file.writeText("\t</modelPatches>\n"));

	ANKI_CHECK(file.writeText("</model>\n"));

	return Error::NONE;
}

template<typename T>
class GltfAnimKey
{
public:
	Second m_time;
	T m_value;
};

class GltfAnimChannel
{
public:
	StringAuto m_name;
	DynamicArrayAuto<GltfAnimKey<Vec3>> m_positions;
	DynamicArrayAuto<GltfAnimKey<Quat>> m_rotations;
	DynamicArrayAuto<GltfAnimKey<F32>> m_scales;

	GltfAnimChannel(GenericMemoryPoolAllocator<U8> alloc)
		: m_name(alloc)
		, m_positions(alloc)
		, m_rotations(alloc)
		, m_scales(alloc)
	{
	}
};

/// Optimize out same animation keys.
template<typename T, typename TZeroFunc, typename TLerpFunc>
static void optimizeChannel(DynamicArrayAuto<GltfAnimKey<T>>& arr, const T& identity, TZeroFunc isZeroFunc,
							TLerpFunc lerpFunc)
{
	if(arr.getSize() < 3)
	{
		return;
	}

	DynamicArrayAuto<GltfAnimKey<T>> newArr(arr.getAllocator());
	newArr.emplaceBack(arr.getFront());
	for(U32 i = 1; i < arr.getSize() - 1; ++i)
	{
		const GltfAnimKey<T>& left = arr[i - 1];
		const GltfAnimKey<T>& middle = arr[i];
		const GltfAnimKey<T>& right = arr[i + 1];

		if(left.m_value == middle.m_value && middle.m_value == right.m_value)
		{
			// Skip it
		}
		else
		{
			const F32 factor = F32((middle.m_time - left.m_time) / (right.m_time - left.m_time));
			ANKI_ASSERT(factor > 0.0f && factor < 1.0f);
			const T lerpRez = lerpFunc(left.m_value, right.m_value, factor);
			if(isZeroFunc(middle.m_value - lerpRez))
			{
				// It's redundant, skip it
			}
			else
			{
				newArr.emplaceBack(middle);
			}
		}
	}
	newArr.emplaceBack(arr.getBack());
	ANKI_ASSERT(newArr.getSize() <= arr.getSize());

	// Check if identity
	if(newArr.getSize() == 2 && isZeroFunc(newArr[0].m_value - newArr[1].m_value)
	   && isZeroFunc(newArr[0].m_value - identity))
	{
		newArr.destroy();
	}

	arr.destroy();
	arr = std::move(newArr);
}

Error GltfImporter::writeAnimation(const cgltf_animation& anim)
{
	StringAuto fname(m_alloc);
	fname.sprintf("%s%s", m_outDir.cstr(), computeAnimationResourceFilename(anim).cstr());
	fname = fixFilename(fname);
	ANKI_IMPORTER_LOGV("Importing animation %s", fname.cstr());

	// Gather the channels
	HashMapAuto<CString, Array<const cgltf_animation_channel*, 3>> channelMap(m_alloc);
	U32 channelCount = 0;
	for(U i = 0; i < anim.channels_count; ++i)
	{
		const cgltf_animation_channel& channel = anim.channels[i];
		const StringAuto channelName = getNodeName(*channel.target_node);

		U idx;
		switch(channel.target_path)
		{
		case cgltf_animation_path_type_translation:
			idx = 0;
			break;
		case cgltf_animation_path_type_rotation:
			idx = 1;
			break;
		case cgltf_animation_path_type_scale:
			idx = 2;
			break;
		default:
			ANKI_ASSERT(0);
			idx = 0;
		}

		auto it = channelMap.find(channelName.toCString());
		if(it != channelMap.getEnd())
		{
			(*it)[idx] = &channel;
		}
		else
		{
			Array<const cgltf_animation_channel*, 3> arr = {};
			arr[idx] = &channel;
			channelMap.emplace(channelName.toCString(), arr);
			++channelCount;
		}
	}

	// Gather the keys
	DynamicArrayAuto<GltfAnimChannel> tempChannels(m_alloc, channelCount, m_alloc);
	channelCount = 0;
	for(auto it = channelMap.getBegin(); it != channelMap.getEnd(); ++it)
	{
		Array<const cgltf_animation_channel*, 3> arr = *it;
		const cgltf_animation_channel& anyChannel = (arr[0]) ? *arr[0] : ((arr[1]) ? *arr[1] : *arr[2]);
		const StringAuto channelName = getNodeName(*anyChannel.target_node);

		tempChannels[channelCount].m_name = channelName;

		// Positions
		if(arr[0])
		{
			const cgltf_animation_channel& channel = *arr[0];
			DynamicArrayAuto<F32> keys(m_alloc);
			readAccessor(*channel.sampler->input, keys);
			DynamicArrayAuto<Vec3> positions(m_alloc);
			readAccessor(*channel.sampler->output, positions);
			if(keys.getSize() != positions.getSize())
			{
				ANKI_IMPORTER_LOGE("Position count should match they keyframes");
				return Error::USER_DATA;
			}

			for(U32 i = 0; i < keys.getSize(); ++i)
			{
				GltfAnimKey<Vec3> key;
				key.m_time = keys[i];
				key.m_value = Vec3(positions[i].x(), positions[i].y(), positions[i].z());

				tempChannels[channelCount].m_positions.emplaceBack(key);
			}
		}

		// Rotations
		if(arr[1])
		{
			const cgltf_animation_channel& channel = *arr[1];
			DynamicArrayAuto<F32> keys(m_alloc);
			readAccessor(*channel.sampler->input, keys);
			DynamicArrayAuto<Quat> rotations(m_alloc);
			readAccessor(*channel.sampler->output, rotations);
			if(keys.getSize() != rotations.getSize())
			{
				ANKI_IMPORTER_LOGE("Rotation count should match they keyframes");
				return Error::USER_DATA;
			}

			for(U32 i = 0; i < keys.getSize(); ++i)
			{
				GltfAnimKey<Quat> key;
				key.m_time = keys[i];
				key.m_value = Quat(rotations[i].x(), rotations[i].y(), rotations[i].z(), rotations[i].w());

				tempChannels[channelCount].m_rotations.emplaceBack(key);
			}
		}

		// Scales
		if(arr[2])
		{
			const cgltf_animation_channel& channel = *arr[2];
			DynamicArrayAuto<F32> keys(m_alloc);
			readAccessor(*channel.sampler->input, keys);
			DynamicArrayAuto<Vec3> scales(m_alloc);
			readAccessor(*channel.sampler->output, scales);
			if(keys.getSize() != scales.getSize())
			{
				ANKI_IMPORTER_LOGE("Scale count should match they keyframes");
				return Error::USER_DATA;
			}

			for(U32 i = 0; i < keys.getSize(); ++i)
			{
				const F32 scaleEpsilon = 0.0001f;
				if(absolute(scales[i][0] - scales[i][1]) > scaleEpsilon
				   || absolute(scales[i][0] - scales[i][2]) > scaleEpsilon)
				{
					ANKI_IMPORTER_LOGE("Expecting uniform scale");
					return Error::USER_DATA;
				}

				GltfAnimKey<F32> key;
				key.m_time = keys[i];
				key.m_value = scales[i][0];

				if(absolute(key.m_value - 1.0f) <= scaleEpsilon)
				{
					key.m_value = 1.0f;
				}

				tempChannels[channelCount].m_scales.emplaceBack(key);
			}
		}

		++channelCount;
	}

	// Optimize animation
	constexpr F32 KILL_EPSILON = 0.001f; // 1 millimiter
	for(GltfAnimChannel& channel : tempChannels)
	{
		optimizeChannel(
			channel.m_positions, Vec3(0.0f),
			[&](const Vec3& a) -> Bool {
				return a.abs() < KILL_EPSILON;
			},
			[&](const Vec3& a, const Vec3& b, F32 u) -> Vec3 {
				return linearInterpolate(a, b, u);
			});
		optimizeChannel(
			channel.m_rotations, Quat::getIdentity(),
			[&](const Quat& a) -> Bool {
				return a.abs() < Quat(EPSILON * 20.0f);
			},
			[&](const Quat& a, const Quat& b, F32 u) -> Quat {
				return a.slerp(b, u);
			});
		optimizeChannel(
			channel.m_scales, 1.0f,
			[&](const F32& a) -> Bool {
				return absolute(a) < KILL_EPSILON;
			},
			[&](const F32& a, const F32& b, F32 u) -> F32 {
				return linearInterpolate(a, b, u);
			});
	}

	// Write file
	File file;
	ANKI_CHECK(file.open(fname.toCString(), FileOpenFlag::WRITE));

	ANKI_CHECK(file.writeTextf("%s\n<animation>\n", XML_HEADER));
	ANKI_CHECK(file.writeText("\t<channels>\n"));

	for(const GltfAnimChannel& channel : tempChannels)
	{
		ANKI_CHECK(file.writeTextf("\t\t<channel name=\"%s\">\n", channel.m_name.cstr()));

		// Positions
		if(channel.m_positions.getSize())
		{
			ANKI_CHECK(file.writeText("\t\t\t<positionKeys>\n"));
			for(const GltfAnimKey<Vec3>& key : channel.m_positions)
			{
				ANKI_CHECK(file.writeTextf("\t\t\t\t<key time=\"%f\">%f %f %f</key>\n", key.m_time, key.m_value.x(),
										   key.m_value.y(), key.m_value.z()));
			}
			ANKI_CHECK(file.writeText("\t\t\t</positionKeys>\n"));
		}

		// Rotations
		if(channel.m_rotations.getSize())
		{
			ANKI_CHECK(file.writeText("\t\t\t<rotationKeys>\n"));
			for(const GltfAnimKey<Quat>& key : channel.m_rotations)
			{
				ANKI_CHECK(file.writeTextf("\t\t\t\t<key time=\"%f\">%f %f %f %f</key>\n", key.m_time, key.m_value.x(),
										   key.m_value.y(), key.m_value.z(), key.m_value.w()));
			}
			ANKI_CHECK(file.writeText("\t\t\t</rotationKeys>\n"));
		}

		// Scales
		if(channel.m_scales.getSize())
		{
			ANKI_CHECK(file.writeText("\t\t\t<scaleKeys>\n"));
			for(const GltfAnimKey<F32>& key : channel.m_scales)
			{
				ANKI_CHECK(file.writeTextf("\t\t\t\t<key time=\"%f\">%f</key>\n", key.m_time, key.m_value));
			}
			ANKI_CHECK(file.writeText("\t\t\t</scaleKeys>\n"));
		}

		ANKI_CHECK(file.writeText("\t\t</channel>\n"));
	}

	ANKI_CHECK(file.writeText("\t</channels>\n"));
	ANKI_CHECK(file.writeText("</animation>\n"));

	return Error::NONE;
}

Error GltfImporter::writeSkeleton(const cgltf_skin& skin)
{
	StringAuto fname(m_alloc);
	fname.sprintf("%s%s", m_outDir.cstr(), computeSkeletonResourceFilename(skin).cstr());
	ANKI_IMPORTER_LOGV("Importing skeleton %s", fname.cstr());

	// Get matrices
	DynamicArrayAuto<Mat4> boneMats(m_alloc);
	readAccessor(*skin.inverse_bind_matrices, boneMats);
	if(boneMats.getSize() != skin.joints_count)
	{
		ANKI_IMPORTER_LOGE("Bone matrices should match the joint count");
		return Error::USER_DATA;
	}

	// Write file
	File file;
	ANKI_CHECK(file.open(fname.toCString(), FileOpenFlag::WRITE));

	ANKI_CHECK(file.writeTextf("%s\n<skeleton>\n", XML_HEADER));
	ANKI_CHECK(file.writeTextf("\t<bones>\n"));

	for(U32 i = 0; i < skin.joints_count; ++i)
	{
		const cgltf_node& boneNode = *skin.joints[i];

		StringAuto parent(m_alloc);

		// Name & parent
		ANKI_CHECK(file.writeTextf("\t\t<bone name=\"%s\" ", getNodeName(boneNode).cstr()));
		if(boneNode.parent && getNodeName(*boneNode.parent) != skin.name)
		{
			ANKI_CHECK(file.writeTextf("parent=\"%s\" ", getNodeName(*boneNode.parent).cstr()));
		}

		// Bone transform
		ANKI_CHECK(file.writeText("boneTransform=\""));
		Mat4 btrf(&boneMats[i][0]);
		btrf.transpose();
		for(U32 j = 0; j < 16; j++)
		{
			ANKI_CHECK(file.writeTextf("%f ", btrf[j]));
		}
		ANKI_CHECK(file.writeText("\" "));

		// Transform
		Transform trf;
		ANKI_CHECK(getNodeTransform(boneNode, trf));
		Mat4 mat{trf};
		ANKI_CHECK(file.writeText("transform=\""));
		for(U j = 0; j < 16; j++)
		{
			ANKI_CHECK(file.writeTextf("%f ", mat[j]));
		}
		ANKI_CHECK(file.writeText("\" "));

		ANKI_CHECK(file.writeText("/>\n"));
	}

	ANKI_CHECK(file.writeText("\t</bones>\n"));
	ANKI_CHECK(file.writeText("</skeleton>\n"));

	return Error::NONE;
}

Error GltfImporter::writeLight(const cgltf_node& node, const HashMapAuto<CString, StringAuto>& parentExtras)
{
	const cgltf_light& light = *node.light;
	StringAuto nodeName = getNodeName(node);
	ANKI_IMPORTER_LOGV("Importing light %s", nodeName.cstr());

	HashMapAuto<CString, StringAuto> extras(parentExtras);
	ANKI_CHECK(getExtras(light.extras, extras));

	CString lightTypeStr;
	switch(light.type)
	{
	case cgltf_light_type_point:
		lightTypeStr = "Point";
		break;
	case cgltf_light_type_spot:
		lightTypeStr = "Spot";
		break;
	case cgltf_light_type_directional:
		lightTypeStr = "Directional";
		break;
	default:
		ANKI_IMPORTER_LOGE("Unsupporter light type %d", light.type);
		return Error::USER_DATA;
	}

	ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:new%sLightNode(\"%s\")\n", lightTypeStr.cstr(), nodeName.cstr()));
	ANKI_CHECK(m_sceneFile.writeText("lcomp = node:getSceneNodeBase():getLightComponent()\n"));

	Vec3 color(light.color[0], light.color[1], light.color[2]);
	color *= light.intensity;
	color *= m_lightIntensityScale;
	ANKI_CHECK(
		m_sceneFile.writeTextf("lcomp:setDiffuseColor(Vec4.new(%f, %f, %f, 1))\n", color.x(), color.y(), color.z()));

	auto shadow = extras.find("shadow");
	if(shadow != extras.getEnd())
	{
		if(*shadow == "true" || *shadow == "1")
		{
			ANKI_CHECK(m_sceneFile.writeText("lcomp:setShadowEnabled(1)\n"));
		}
		else
		{
			ANKI_CHECK(m_sceneFile.writeText("lcomp:setShadowEnabled(0)\n"));
		}
	}

	if(light.type == cgltf_light_type_point)
	{
		ANKI_CHECK(m_sceneFile.writeTextf("lcomp:setRadius(%f)\n",
										  (light.range > 0.0f) ? light.range : computeLightRadius(color)));
	}
	else if(light.type == cgltf_light_type_spot)
	{
		ANKI_CHECK(m_sceneFile.writeTextf("lcomp:setDistance(%f)\n",
										  (light.range > 0.0f) ? light.range : computeLightRadius(color)));

		const F32 outer = light.spot_outer_cone_angle * 2.0f;
		ANKI_CHECK(m_sceneFile.writeTextf("lcomp:setOuterAngle(%f)\n", outer));

		auto angStr = extras.find("inner_cone_angle_factor");
		F32 inner;
		if(angStr != extras.getEnd())
		{
			F32 factor;
			ANKI_CHECK(angStr->toNumber(factor));
			inner = light.spot_inner_cone_angle * 2.0f * min(1.0f, factor);
		}
		else
		{
			inner = light.spot_inner_cone_angle * 2.0f;
		}

		if(inner >= 0.95f * outer)
		{
			inner = 0.75f * outer;
		}

		ANKI_CHECK(m_sceneFile.writeTextf("lcomp:setInnerAngle(%f)\n", inner));
	}

	auto lensFlaresFname = extras.find("lens_flare");
	if(lensFlaresFname != extras.getEnd())
	{
		ANKI_CHECK(m_sceneFile.writeTextf("lfcomp = node:getSceneNodeBase():getLensFlareComponent()\n"));
		ANKI_CHECK(m_sceneFile.writeTextf("lfcomp:loadImageResource(\"%s\")\n", lensFlaresFname->cstr()));

		auto lsSpriteSize = extras.find("lens_flare_first_sprite_size");
		auto lsColor = extras.find("lens_flare_color");

		if(lsSpriteSize != extras.getEnd())
		{
			DynamicArrayAuto<F64> numbers(m_alloc);
			const U32 count = 2;
			ANKI_CHECK(parseArrayOfNumbers(lsSpriteSize->toCString(), numbers, &count));

			ANKI_CHECK(m_sceneFile.writeTextf("lfcomp:setFirstFlareSize(Vec2.new(%f, %f))\n", numbers[0], numbers[1]));
		}

		if(lsColor != extras.getEnd())
		{
			DynamicArrayAuto<F64> numbers(m_alloc);
			const U32 count = 4;
			ANKI_CHECK(parseArrayOfNumbers(lsColor->toCString(), numbers, &count));

			ANKI_CHECK(m_sceneFile.writeTextf("lfcomp:setColorMultiplier(Vec4.new(%f, %f, %f, %f))\n", numbers[0],
											  numbers[1], numbers[2], numbers[3]));
		}
	}

	auto lightEventIntensity = extras.find("light_event_intensity");
	auto lightEventFrequency = extras.find("light_event_frequency");
	if(lightEventIntensity != extras.getEnd() || lightEventFrequency != extras.getEnd())
	{
		ANKI_CHECK(m_sceneFile.writeText("event = events:newLightEvent(0.0, -1.0, node:getSceneNodeBase())\n"));

		if(lightEventIntensity != extras.getEnd())
		{
			DynamicArrayAuto<F64> numbers(m_alloc);
			const U32 count = 4;
			ANKI_CHECK(parseArrayOfNumbers(lightEventIntensity->toCString(), numbers, &count));
			ANKI_CHECK(m_sceneFile.writeTextf("event:setIntensityMultiplier(Vec4.new(%f, %f, %f, %f))\n", numbers[0],
											  numbers[1], numbers[2], numbers[3]));
		}

		if(lightEventFrequency != extras.getEnd())
		{
			DynamicArrayAuto<F64> numbers(m_alloc);
			const U32 count = 2;
			ANKI_CHECK(parseArrayOfNumbers(lightEventFrequency->toCString(), numbers, &count));
			ANKI_CHECK(m_sceneFile.writeTextf("event:setFrequency(%f, %f)\n", numbers[0], numbers[1]));
		}
	}

	return Error::NONE;
}

Error GltfImporter::writeCamera(const cgltf_node& node,
								[[maybe_unused]] const HashMapAuto<CString, StringAuto>& parentExtras)
{
	if(node.camera->type != cgltf_camera_type_perspective)
	{
		ANKI_IMPORTER_LOGV("Unsupported camera type: %s", getNodeName(node).cstr());
		return Error::NONE;
	}

	const cgltf_camera_perspective& cam = node.camera->data.perspective;
	ANKI_IMPORTER_LOGV("Importing camera %s", getNodeName(node).cstr());

	ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:newPerspectiveCameraNode(\"%s\")\n", getNodeName(node).cstr()));
	ANKI_CHECK(m_sceneFile.writeText("scene:setActiveCameraNode(node:getSceneNodeBase())\n"));
	ANKI_CHECK(m_sceneFile.writeText("frustumc = node:getSceneNodeBase():getFrustumComponent()\n"));

	ANKI_CHECK(m_sceneFile.writeTextf("frustumc:setPerspective(%f, %f, getMainRenderer():getAspectRatio() * %f, %f)\n",
									  cam.znear, cam.zfar, cam.yfov, cam.yfov));
	ANKI_CHECK(m_sceneFile.writeText("frustumc:setShadowCascadesDistancePower(1.5)\n"));
	ANKI_CHECK(m_sceneFile.writeTextf("frustumc:setEffectiveShadowDistance(%f)\n", min(cam.zfar, 100.0f)));

	return Error::NONE;
}

Error GltfImporter::writeModelNode(const cgltf_node& node, const HashMapAuto<CString, StringAuto>& parentExtras)
{
	ANKI_IMPORTER_LOGV("Importing model node %s", getNodeName(node).cstr());

	HashMapAuto<CString, StringAuto> extras(parentExtras);
	ANKI_CHECK(getExtras(node.extras, extras));

	const StringAuto modelFname = computeModelResourceFilename(*node.mesh);

	ANKI_CHECK(m_sceneFile.writeTextf("\nnode = scene:newModelNode(\"%s\")\n", getNodeName(node).cstr()));
	ANKI_CHECK(m_sceneFile.writeTextf("node:getSceneNodeBase():getModelComponent():loadModelResource(\"%s%s\")\n",
									  m_rpath.cstr(), modelFname.cstr()));

	if(node.skin)
	{
		ANKI_CHECK(m_sceneFile.writeTextf("node:getSceneNodeBase():getSkinComponent():loadSkeletonResource(\"%s%s\")\n",
										  m_rpath.cstr(), computeSkeletonResourceFilename(*node.skin).cstr()));
	}

	return Error::NONE;
}

StringAuto GltfImporter::computeModelResourceFilename(const cgltf_mesh& mesh) const
{
	StringListAuto list(m_alloc);

	list.pushBack(mesh.name);

	for(U i = 0; i < mesh.primitives_count; ++i)
	{
		list.pushBackSprintf("_%s", mesh.primitives[i].material->name);
	}

	StringAuto joined(m_alloc);
	list.join("", joined);

	const U64 hash = computeHash(joined.getBegin(), joined.getLength());

	StringAuto out(m_alloc);
	out.sprintf("%.64s_%" PRIx64 ".ankimdl", joined.cstr(), hash); // Limit the filename size

	return out;
}

StringAuto GltfImporter::computeMeshResourceFilename(const cgltf_mesh& mesh, U32 lod) const
{
	const U64 hash = computeHash(mesh.name, strlen(mesh.name));

	StringAuto out(m_alloc);

	out.sprintf("%.64s_lod%u_%" PRIx64 ".ankimesh", mesh.name, lod, hash); // Limit the filename size

	return out;
}

StringAuto GltfImporter::computeMaterialResourceFilename(const cgltf_material& mtl) const
{
	const U64 hash = computeHash(mtl.name, strlen(mtl.name));

	StringAuto out(m_alloc);

	out.sprintf("%.64s_%" PRIx64 ".ankimtl", mtl.name, hash); // Limit the filename size

	return out;
}

StringAuto GltfImporter::computeAnimationResourceFilename(const cgltf_animation& anim) const
{
	const U64 hash = computeHash(anim.name, strlen(anim.name));

	StringAuto out(m_alloc);

	out.sprintf("%.64s_%" PRIx64 ".ankianim", anim.name, hash); // Limit the filename size

	return out;
}

StringAuto GltfImporter::computeSkeletonResourceFilename(const cgltf_skin& skin) const
{
	const U64 hash = computeHash(skin.name, strlen(skin.name));

	StringAuto out(m_alloc);

	out.sprintf("%.64s_%" PRIx64 ".ankiskel", skin.name, hash); // Limit the filename size

	return out;
}

} // end namespace anki
