/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "Precompiled.h"
#pragma hdrstop

extern "C"
{
	float R_NoiseGet4f(float x, float y, float z, float t);
}

#define	WAVEVALUE(table, base, amplitude, phase, freq) ((base) + table[ri.ftol((((phase) + material->time_ * (freq)) * g_funcTableSize)) & g_funcTableMask] * (amplitude))

namespace renderer {

vec4 MaterialStage::getFogColorMask() const
{
	assert(active);

	switch(adjustColorsForFog)
	{
	case MaterialAdjustColorsForFog::ModulateRGB:
		return vec4(1, 1, 1, 0);
	case MaterialAdjustColorsForFog::ModulateAlpha:
		return vec4(0, 0, 0, 1);
	case MaterialAdjustColorsForFog::ModulateRGBA:
		return vec4(1, 1, 1, 1);
	default:
		break;
	}

	return vec4(0, 0, 0, 0);
}

uint64_t MaterialStage::getState() const
{
	assert(active);
	uint64_t state = BGFX_STATE_BLEND_FUNC(blendSrc, blendDst);
	state |= depthTestBits;

	if (depthWrite)
	{
		state |= BGFX_STATE_DEPTH_WRITE;
	}

	if (material->cullType != MaterialCullType::TwoSided)
	{
		bool cullFront = (material->cullType == MaterialCullType::FrontSided);

		if (g_main->isMirrorCamera())
			cullFront = !cullFront;

		state |= cullFront ? BGFX_STATE_CULL_CCW : BGFX_STATE_CULL_CW;
	}

	return state;
}

void MaterialStage::setShaderUniforms(Uniforms_MaterialStage *uniforms, int flags) const
{
	uniforms->alphaTest.set((float)alphaTest);
	uniforms->lightType.set(vec4((float)light, 0, 0, 0));
	uniforms->normalScale.set(normalScale);
	uniforms->specularScale.set(specularScale);

	if (flags & (MaterialStageSetUniformsFlags::ColorGen | MaterialStageSetUniformsFlags::TexGen))
	{
		vec4 generators;
		generators[Uniforms_MaterialStage::Generators::TexCoord] = (float)bundles[0].tcGen;
		generators[Uniforms_MaterialStage::Generators::Color] = (float)rgbGen;
		generators[Uniforms_MaterialStage::Generators::Alpha] = (float)alphaGen;
		uniforms->generators.set(generators);
	}

	if (flags & MaterialStageSetUniformsFlags::ColorGen)
	{
		// rgbGen and alphaGen
		vec4 baseColor, vertexColor;
		calculateColors(&baseColor, &vertexColor);
		uniforms->baseColor.set(baseColor);
		uniforms->vertexColor.set(vertexColor);

		if (alphaGen == MaterialAlphaGen::Portal)
		{
			uniforms->portalRange.set(material->portalRange);
		}
	}

	if (flags & MaterialStageSetUniformsFlags::TexGen)
	{
		// tcGen and tcMod
		vec4 texMatrix, texOffTurb;
		calculateTexMods(&texMatrix, &texOffTurb);
		uniforms->diffuseTextureMatrix.set(texMatrix);
		uniforms->diffuseTextureOffsetTurbulent.set(texOffTurb);

		if (bundles[0].tcGen == MaterialTexCoordGen::Vector)
		{
			uniforms->tcGenVector0.set(bundles[0].tcGenVectors[0]);
			uniforms->tcGenVector1.set(bundles[0].tcGenVectors[1]);
		}
	}
}

void MaterialStage::setTextureSamplers(Uniforms_MaterialStage *uniforms) const
{
	assert(uniforms);
	assert(active);
	setTextureSampler(MaterialTextureBundleIndex::DiffuseMap, uniforms);
	setTextureSampler(MaterialTextureBundleIndex::Lightmap, uniforms);

#if 0
	if (stage.light != MaterialLight::None)
	{
		// bind textures that are sampled and used in the glsl shader, and
		// bind whiteImage to textures that are sampled but zeroed in the glsl shader
		//
		// alternatives:
		//  - use the last bound texture
		//     -> costs more to sample a higher res texture then throw out the result
		//  - disable texture sampling in glsl shader with #ifdefs, as before
		//     -> increases the number of shaders that must be compiled
		const bool phong = g_cvars.normalMapping->integer || g_cvars.specularMapping->integer;
		vec4 enableTextures;

		if (stage.light != MaterialLight::None && phong)
		{
			if (stage.bundles[MaterialTextureBundleIndex::NormalMap].textures[0])
			{
				stage.setTextureSampler(MaterialTextureBundleIndex::NormalMap);
				enableTextures[0] = 1.0f;
			}
			else if (g_cvars.normalMapping->integer)
			{
				g_textureCache->getWhiteTexture()->setSampler(MaterialTextureBundleIndex::NormalMap);
			}

			if (stage.bundles[MaterialTextureBundleIndex::Deluxemap].textures[0])
			{
				stage.setTextureSampler(MaterialTextureBundleIndex::Deluxemap);
				enableTextures[1] = 1.0f;
			}
			else if (g_cvars.deluxeMapping->integer)
			{
				g_textureCache->getWhiteTexture()->setSampler(MaterialTextureBundleIndex::Deluxemap);
			}

			if (stage.bundles[MaterialTextureBundleIndex::Specularmap].textures[0])
			{
				stage.setTextureSampler(MaterialTextureBundleIndex::Specularmap);
				enableTextures[2] = 1.0f;
			}
			else if (g_cvars.specularMapping->integer)
			{
				g_textureCache->getWhiteTexture()->setSampler(MaterialTextureBundleIndex::Specularmap);
			}
		}

		g_main->uniforms->enableTextures.set(enableTextures);
	}
#endif
}

float *MaterialStage::tableForFunc(MaterialWaveformGenFunc func) const
{
	switch(func)
	{
	case MaterialWaveformGenFunc::Sin:
		return g_sinTable;
	case MaterialWaveformGenFunc::Triangle:
		return g_triangleTable;
	case MaterialWaveformGenFunc::Square:
		return g_squareTable;
	case MaterialWaveformGenFunc::Sawtooth:
		return g_sawToothTable;
	case MaterialWaveformGenFunc::InverseSawtooth:
		return g_inverseSawToothTable;
	case MaterialWaveformGenFunc::None:
	default:
		break;
	}

	ri.Error(ERR_DROP, "TableForFunc called with invalid function '%d' in shader '%s'", func, material->name);
	return NULL;
}

float MaterialStage::evaluateWaveForm(const MaterialWaveForm &wf) const
{
	return WAVEVALUE(tableForFunc(wf.func), wf.base, wf.amplitude, wf.phase, wf.frequency);
}

float MaterialStage::evaluateWaveFormClamped(const MaterialWaveForm &wf) const
{
	return math::Clamped(evaluateWaveForm(wf), 0.0f, 1.0f);
}

void MaterialStage::calculateTexMods(vec4 *outMatrix, vec4 *outOffTurb) const
{
	assert(outMatrix);
	assert(outOffTurb);

	float matrix[6] = { 1, 0, 0, 1, 0, 0 };
	float currentMatrix[6] = { 1, 0, 0, 1, 0, 0 };
	(*outMatrix) = { 1, 0, 0, 1 };
	(*outOffTurb) = { 0, 0, 0, 0 };
	const auto &bundle = bundles[0];

	for (int tm = 0; tm < bundle.numTexMods; tm++)
	{
		switch (bundle.texMods[tm].type)
		{
		case MaterialTexMod::None:
			tm = MaterialTextureBundle::maxTexMods; // break out of for loop
			break;

		case MaterialTexMod::Turbulent:
			calculateTurbulentFactors(bundle.texMods[tm].wave, &(*outOffTurb)[2], &(*outOffTurb)[3]);
			break;

		case MaterialTexMod::EntityTranslate:
			calculateScrollTexMatrix(g_main->getCurrentEntity()->e.shaderTexCoord, matrix);
			break;

		case MaterialTexMod::Scroll:
			calculateScrollTexMatrix(bundle.texMods[tm].scroll, matrix);
			break;

		case MaterialTexMod::Scale:
			calculateScaleTexMatrix(bundle.texMods[tm].scale, matrix);
			break;
		
		case MaterialTexMod::Stretch:
			calculateStretchTexMatrix(bundle.texMods[tm].wave,  matrix);
			break;

		case MaterialTexMod::Transform:
			calculateTransformTexMatrix(bundle.texMods[tm], matrix);
			break;

		case MaterialTexMod::Rotate:
			calculateRotateTexMatrix(bundle.texMods[tm].rotateSpeed, matrix);
			break;

		default:
			ri.Error(ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle.texMods[tm].type, material->name);
			break;
		}

		switch (bundle.texMods[tm].type)
		{	
		case MaterialTexMod::None:
		case MaterialTexMod::Turbulent:
		default:
			break;

		case MaterialTexMod::EntityTranslate:
		case MaterialTexMod::Scroll:
		case MaterialTexMod::Scale:
		case MaterialTexMod::Stretch:
		case MaterialTexMod::Transform:
		case MaterialTexMod::Rotate:
			(*outMatrix)[0] = matrix[0] * currentMatrix[0] + matrix[2] * currentMatrix[1];
			(*outMatrix)[1] = matrix[1] * currentMatrix[0] + matrix[3] * currentMatrix[1];
			(*outMatrix)[2] = matrix[0] * currentMatrix[2] + matrix[2] * currentMatrix[3];
			(*outMatrix)[3] = matrix[1] * currentMatrix[2] + matrix[3] * currentMatrix[3];

			(*outOffTurb)[0] = matrix[0] * currentMatrix[4] + matrix[2] * currentMatrix[5] + matrix[4];
			(*outOffTurb)[1] = matrix[1] * currentMatrix[4] + matrix[3] * currentMatrix[5] + matrix[5];

			currentMatrix[0] = (*outMatrix)[0];
			currentMatrix[1] = (*outMatrix)[1];
			currentMatrix[2] = (*outMatrix)[2];
			currentMatrix[3] = (*outMatrix)[3];
			currentMatrix[4] = (*outOffTurb)[0];
			currentMatrix[5] = (*outOffTurb)[1];
			break;
		}
	}
}

void MaterialStage::calculateTurbulentFactors(const MaterialWaveForm &wf, float *amplitude, float *now) const
{
	*now = wf.phase + material->time_ * wf.frequency;
	*amplitude = wf.amplitude;
}

void MaterialStage::calculateScaleTexMatrix(vec2 scale, float *matrix) const
{
	matrix[0] = scale[0]; matrix[2] = 0.0f;     matrix[4] = 0.0f;
	matrix[1] = 0.0f;     matrix[3] = scale[1]; matrix[5] = 0.0f;
}

void MaterialStage::calculateScrollTexMatrix(vec2 scrollSpeed, float *matrix) const
{
	float adjustedScrollS = scrollSpeed[0] * material->time_;
	float adjustedScrollT = scrollSpeed[1] * material->time_;

	// clamp so coordinates don't continuously get larger, causing problems with hardware limits
	adjustedScrollS = adjustedScrollS - floor(adjustedScrollS);
	adjustedScrollT = adjustedScrollT - floor(adjustedScrollT);

	matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = adjustedScrollS;
	matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = adjustedScrollT;
}

void MaterialStage::calculateStretchTexMatrix(const MaterialWaveForm &wf, float *matrix) const
{
	const float p = 1.0f / evaluateWaveForm(wf);
	matrix[0] = p; matrix[2] = 0; matrix[4] = 0.5f - 0.5f * p;
	matrix[1] = 0; matrix[3] = p; matrix[5] = 0.5f - 0.5f * p;
}

void MaterialStage::calculateTransformTexMatrix(const MaterialTexModInfo &tmi, float *matrix) const
{
	matrix[0] = tmi.matrix[0][0]; matrix[2] = tmi.matrix[1][0]; matrix[4] = tmi.translate[0];
	matrix[1] = tmi.matrix[0][1]; matrix[3] = tmi.matrix[1][1]; matrix[5] = tmi.translate[1];
}

void MaterialStage::calculateRotateTexMatrix(float degsPerSecond, float *matrix) const
{
	float degs = -degsPerSecond * material->time_;
	int index = degs * (g_funcTableSize / 360.0f);
	float sinValue = g_sinTable[index & g_funcTableMask];
	float cosValue = g_sinTable[(index + g_funcTableSize / 4) & g_funcTableMask];
	matrix[0] = cosValue; matrix[2] = -sinValue; matrix[4] = 0.5 - 0.5 * cosValue + 0.5 * sinValue;
	matrix[1] = sinValue; matrix[3] = cosValue;  matrix[5] = 0.5 - 0.5 * sinValue - 0.5 * cosValue;
}

float MaterialStage::calculateWaveColorSingle(const MaterialWaveForm &wf) const
{
	float glow;

	if (wf.func == MaterialWaveformGenFunc::Noise)
	{
		glow = wf.base + R_NoiseGet4f(0, 0, 0, (material->time_ + wf.phase ) * wf.frequency ) * wf.amplitude;
	}
	else
	{
		glow = evaluateWaveForm(wf) * g_identityLight;
	}
	
	return math::Clamped(glow, 0.0f, 1.0f);
}

float MaterialStage::calculateWaveAlphaSingle(const MaterialWaveForm &wf) const
{
	return evaluateWaveFormClamped(wf);
}

void MaterialStage::calculateColors(vec4 *baseColor, vec4 *vertColor) const
{
	assert(baseColor);
	assert(vertColor);

	*baseColor = vec4::white;
	*vertColor = vec4(0, 0, 0, 0);

	// rgbGen
	switch (rgbGen)
	{
		case MaterialColorGen::IdentityLighting:
			(*baseColor).r = (*baseColor).g = (*baseColor).b = g_identityLight;
			break;
		case MaterialColorGen::ExactVertex:
		case MaterialColorGen::ExactVertexLit:
			*baseColor = vec4::black;
			*vertColor = vec4::white;
			break;
		case MaterialColorGen::Const:
			(*baseColor).r = constantColor[0] / 255.0f;
			(*baseColor).g = constantColor[1] / 255.0f;
			(*baseColor).b = constantColor[2] / 255.0f;
			(*baseColor).a = constantColor[3] / 255.0f;
			break;
		case MaterialColorGen::Vertex:
			*baseColor = vec4::black;
			*vertColor = vec4(g_identityLight, g_identityLight, g_identityLight, 1);
			break;
		case MaterialColorGen::VertexLit:
			*baseColor = vec4::black;
			*vertColor = vec4(g_identityLight);
			break;
		case MaterialColorGen::OneMinusVertex:
			(*baseColor).r = (*baseColor).g = (*baseColor).b = g_identityLight;
			(*vertColor).r = (*vertColor).g = (*vertColor).b = -g_identityLight;
			break;
		case MaterialColorGen::Fog:
			/*{
				fog_t		*fog;

				fog = tr.world->fogs + tess.fogNum;

				(*baseColor).r = ((unsigned char *)(&fog->colorInt)).r / 255.0f;
				(*baseColor).g = ((unsigned char *)(&fog->colorInt)).g / 255.0f;
				(*baseColor).b = ((unsigned char *)(&fog->colorInt)).b / 255.0f;
				(*baseColor).a = ((unsigned char *)(&fog->colorInt)).a / 255.0f;
			}*/
			break;
		case MaterialColorGen::Waveform:
			(*baseColor).r = (*baseColor).g = (*baseColor).b = calculateWaveColorSingle(rgbWave);
			break;
		case MaterialColorGen::Entity:
			if (g_main->getCurrentEntity())
			{
				(*baseColor).r = g_main->getCurrentEntity()->e.shaderRGBA[0] / 255.0f;
				(*baseColor).g = g_main->getCurrentEntity()->e.shaderRGBA[1] / 255.0f;
				(*baseColor).b = g_main->getCurrentEntity()->e.shaderRGBA[2] / 255.0f;
				(*baseColor).a = g_main->getCurrentEntity()->e.shaderRGBA[3] / 255.0f;
			}
			break;
		case MaterialColorGen::OneMinusEntity:
			if (g_main->getCurrentEntity())
			{
				(*baseColor).r = 1.0f - g_main->getCurrentEntity()->e.shaderRGBA[0] / 255.0f;
				(*baseColor).g = 1.0f - g_main->getCurrentEntity()->e.shaderRGBA[1] / 255.0f;
				(*baseColor).b = 1.0f - g_main->getCurrentEntity()->e.shaderRGBA[2] / 255.0f;
				(*baseColor).a = 1.0f - g_main->getCurrentEntity()->e.shaderRGBA[3] / 255.0f;
			}
			break;
		case MaterialColorGen::Identity:
		case MaterialColorGen::LightingDiffuse:
		case MaterialColorGen::Bad:
			break;
	}

	// alphaGen
	switch (alphaGen)
	{
		case MaterialAlphaGen::Skip:
			break;
		case MaterialAlphaGen::Const:
			(*baseColor).a = constantColor[3] / 255.0f;
			(*vertColor).a = 0.0f;
			break;
		case MaterialAlphaGen::Waveform:
			(*baseColor).a = calculateWaveAlphaSingle(alphaWave);
			(*vertColor).a = 0.0f;
			break;
		case MaterialAlphaGen::Entity:
			if (g_main->getCurrentEntity())
			{
				(*baseColor).a = g_main->getCurrentEntity()->e.shaderRGBA[3] / 255.0f;
			}
			(*vertColor).a = 0.0f;
			break;
		case MaterialAlphaGen::OneMinusEntity:
			if (g_main->getCurrentEntity())
			{
				(*baseColor).a = 1.0f - g_main->getCurrentEntity()->e.shaderRGBA[3] / 255.0f;
			}
			(*vertColor).a = 0.0f;
			break;
		case MaterialAlphaGen::Vertex:
			(*baseColor).a = 0.0f;
			(*vertColor).a = 1.0f;
			break;
		case MaterialAlphaGen::OneMinusVertex:
			(*baseColor).a = 1.0f;
			(*vertColor).a = -1.0f;
			break;
		case MaterialAlphaGen::Identity:
		case MaterialAlphaGen::LightingSpecular:
		case MaterialAlphaGen::Portal:
			// Done entirely in vertex program
			(*baseColor).a = 1.0f;
			(*vertColor).a = 0.0f;
			break;
	}

	// Multiply color by overbrightbits if this isn't a blend.
	if (g_overbrightFactor > 1
		&& blendSrc != BGFX_STATE_BLEND_DST_COLOR
		&& blendSrc != BGFX_STATE_BLEND_INV_DST_COLOR
		&& blendDst != BGFX_STATE_BLEND_SRC_COLOR
		&& blendDst != BGFX_STATE_BLEND_INV_SRC_COLOR)
	{
		(*baseColor) = vec4(baseColor->xyz() * g_overbrightFactor, baseColor->a);
		(*vertColor) = vec4(vertColor->xyz() * g_overbrightFactor, vertColor->a);
	}
}

void MaterialStage::setTextureSampler(int sampler, Uniforms_MaterialStage *uniforms) const
{
	assert(uniforms);
	assert(active);
	auto &bundle = bundles[sampler];

	if (bundle.isVideoMap)
	{
		ri.CIN_RunCinematic(bundle.videoMapHandle);
		ri.CIN_UploadCinematic(bundle.videoMapHandle);
	}

	if (!bundle.textures[0])
		return;

	if (bundle.numImageAnimations <= 1)
	{
		bgfx::setTexture(sampler, uniforms->textures[sampler]->handle, bundle.textures[0]->getHandle());
	}
	else
	{
		// It is necessary to do this messy calc to make sure animations line up exactly with waveforms of the same frequency.
		int index = ri.ftol(material->time_ * bundle.imageAnimationSpeed * g_funcTableSize);
		index >>= g_funcTableSize2;
		index = std::max(0, index); // May happen with shader time offsets.
		index %= bundle.numImageAnimations;
		bgfx::setTexture(sampler, uniforms->textures[sampler]->handle, bundle.textures[index]->getHandle());
	}
}

float Material::setTime(float time)
{
	time_ = time - timeOffset;

	if (g_main->getCurrentEntity())
	{
		time_ -= g_main->getCurrentEntity()->e.shaderTime;
	}

	return time_;
}

void Material::doCpuDeforms(DrawCall *dc, const mat3 &sceneRotation) const
{
	assert(dc);

	if (!hasCpuDeforms() || dc->vb.type != DrawCall::BufferType::Transient || dc->ib.type != DrawCall::BufferType::Transient)
		return;

	auto vertices = (Vertex *)dc->vb.transientHandle.data;
	auto indices = (uint16_t *)dc->ib.transientHandle.data;
	const size_t nIndices = dc->ib.nIndices;

	for (auto &ds : deforms)
	{
		if (!isCpuDeform(ds.deformation))
			continue;

		switch (ds.deformation)
		{
		// Assuming the geometry is triangulated quads.
		// Autosprite will rebuild them as forward facing sprites.
		// Autosprite2 will pivot a rectangular quad along the center of its long axis.
		case MaterialDeform::Autosprite:
		case MaterialDeform::Autosprite2:
		{
			if ((nIndices % 6) != 0)
			{
				ri.Printf(PRINT_WARNING, "Autosprite material %s had odd index count %d\n", name, (int)nIndices);
			}

			vec3 forward, leftDir, upDir;

			if (g_main->getCurrentEntity())
			{
				forward.x = vec3::dotProduct(sceneRotation[0], g_main->getCurrentEntity()->e.axis[0]);
				forward.y = vec3::dotProduct(sceneRotation[0], g_main->getCurrentEntity()->e.axis[1]);
				forward.z = vec3::dotProduct(sceneRotation[0], g_main->getCurrentEntity()->e.axis[2]);
				leftDir.x = vec3::dotProduct(sceneRotation[1], g_main->getCurrentEntity()->e.axis[0]);
				leftDir.y = vec3::dotProduct(sceneRotation[1], g_main->getCurrentEntity()->e.axis[1]);
				leftDir.z = vec3::dotProduct(sceneRotation[1], g_main->getCurrentEntity()->e.axis[2]);
				upDir.x = vec3::dotProduct(sceneRotation[2], g_main->getCurrentEntity()->e.axis[0]);
				upDir.y = vec3::dotProduct(sceneRotation[2], g_main->getCurrentEntity()->e.axis[1]);
				upDir.z = vec3::dotProduct(sceneRotation[2], g_main->getCurrentEntity()->e.axis[2]);
			}
			else
			{
				forward = sceneRotation[0];
				leftDir = sceneRotation[1];
				upDir = sceneRotation[2];
			}

			// Iterate through triangulated quads.
			for (size_t quadIndex = 0; quadIndex < nIndices / 6; quadIndex++)
			{
				const size_t firstIndex = dc->ib.firstIndex + quadIndex * 6;

				// Get the quad corner vertices and their indexes.
				auto v = ExtractQuadCorners(vertices, &indices[firstIndex]);
				std::array<uint16_t, 4> vi;

				for (size_t i = 0; i < vi.size(); i++)
					vi[i] = uint16_t(v[i] - vertices);

				// Find the midpoint.
				const vec3 midpoint = (v[0]->pos + v[1]->pos + v[2]->pos + v[3]->pos) * 0.25f;
				const float radius = (v[0]->pos - midpoint).length() * 0.707f; // / sqrt(2)

				if (g_cvars.softSprites->integer)
				{
					// Assumes all quads in this drawcall have the same radius.
					dc->softSpriteDepth = radius / 2.0f;
				}

				if (ds.deformation == MaterialDeform::Autosprite)
				{
					vec3 left(leftDir * radius);
					vec3 up(upDir * radius);

					if (g_main->isMirrorCamera())
						left = -left;

					// Compensate for scale in the axes if necessary.
					if (g_main->getCurrentEntity() && g_main->getCurrentEntity()->e.nonNormalizedAxes)
					{
						float axisLength = vec3(g_main->getCurrentEntity()->e.axis[0]).length();

						if (!axisLength)
						{
							axisLength = 0;
						}
						else
						{
							axisLength = 1.0f / axisLength;
						}

						left *= axisLength;
						up *= axisLength;
					}

					// Rebuild quad facing the main camera.
					v[0]->pos = midpoint + left + up;
					v[1]->pos = midpoint - left + up;
					v[2]->pos = midpoint - left - up;
					v[3]->pos = midpoint + left - up;

					// Constant normal all the way around.
					v[0]->normal = v[1]->normal = v[2]->normal = v[3]->normal = -sceneRotation[0];

					// Standard square texture coordinates.
					v[0]->texCoord = v[0]->texCoord2 = vec2(0, 0);
					v[1]->texCoord = v[1]->texCoord2 = vec2(1, 0);
					v[2]->texCoord = v[2]->texCoord2 = vec2(1, 1);
					v[3]->texCoord = v[3]->texCoord2 = vec2(0, 1);

					indices[firstIndex + 0] = vi[0];
					indices[firstIndex + 1] = vi[1];
					indices[firstIndex + 2] = vi[3];
					indices[firstIndex + 3] = vi[3];
					indices[firstIndex + 4] = vi[1];
					indices[firstIndex + 5] = vi[2];
				}
				else if (ds.deformation == MaterialDeform::Autosprite2)
				{
					const int edgeVerts[6][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 } };
					uint16_t smallestIndex = indices[firstIndex];

					for (size_t i = 0; i < vi.size(); i++)
					{
						smallestIndex = std::min(smallestIndex, vi[i]);
					}

					// Identify the two shortest edges.
					int nums[2] = {};
					float lengths[2];
					lengths[0] = lengths[1] = 999999;

					for (int i = 0; i < 6; i++)
					{
						const vec3 temp = vec3(v[edgeVerts[i][0]]->pos) - vec3(v[edgeVerts[i][1]]->pos);
						const float l = vec3::dotProduct(temp, temp);

						if (l < lengths[0])
						{
							nums[1] = nums[0];
							lengths[1] = lengths[0];
							nums[0] = i;
							lengths[0] = l;
						}
						else if (l < lengths[1])
						{
							nums[1] = i;
							lengths[1] = l;
						}
					}

					// Find the midpoints.
					vec3 midpoints[2];

					for (int i = 0; i < 2 ; i++)
					{
						midpoints[i] = (v[edgeVerts[nums[i]][0]]->pos + v[edgeVerts[nums[i]][1]]->pos) * 0.5f;
					}

					// Find the vector of the major axis.
					const vec3 major(midpoints[1] - midpoints[0]);

					// Cross this with the view direction to get minor axis.
					const vec3 minor(vec3::crossProduct(major, forward).normal());
		
					// Re-project the points.
					for (int i = 0; i < 2; i++)
					{
						// We need to see which direction this edge is used to determine direction of projection.
						int j;

						for (j = 0 ; j < 5 ; j++)
						{
							if (indices[firstIndex + j] == smallestIndex + edgeVerts[nums[i]][0] && indices[firstIndex + j + 1] == smallestIndex + edgeVerts[nums[i]][1])
								break;
						}

						const float l = 0.5f * sqrt(lengths[i]);
						vec3 *v1 = &v[edgeVerts[nums[i]][0]]->pos;
						vec3 *v2 = &v[edgeVerts[nums[i]][1]]->pos;

						if (j == 5)
						{
							*v1 = midpoints[i] + minor * l;
							*v2 = midpoints[i] + minor * -l;
						}
						else
						{
							*v1 = midpoints[i] + minor * -l;
							*v2 = midpoints[i] + minor * l;
						}
					}
				}
			}
			break;
		}

		case MaterialDeform::Normals:
		case MaterialDeform::Text0:
		case MaterialDeform::Text1:
		case MaterialDeform::Text2:
		case MaterialDeform::Text3:
		case MaterialDeform::Text4:
		case MaterialDeform::Text5:
		case MaterialDeform::Text6:
		case MaterialDeform::Text7:
			break;
		}
	}
}

void Material::setDeformUniforms(Uniforms_Material *uniforms) const
{
	assert(uniforms);

	if (!hasGpuDeforms())
	{
		uniforms->nDeforms.set(vec4::empty);
		return;
	}

	vec4 moveDirs[maxDeforms];
	vec4 gen_Wave_Base_Amplitude[maxDeforms];
	vec4 frequency_Phase_Spread[maxDeforms];
	uint16_t i = 0;

	for (auto &ds : deforms)
	{
		if (!isGpuDeform(ds.deformation))
			continue;

		switch (ds.deformation)
		{
		case MaterialDeform::Wave:
			gen_Wave_Base_Amplitude[i] = vec4((float)ds.deformation, (float)ds.deformationWave.func, ds.deformationWave.base, ds.deformationWave.amplitude);
			frequency_Phase_Spread[i] = vec4(ds.deformationWave.frequency, ds.deformationWave.phase, ds.deformationSpread, 0);
			break;

		case MaterialDeform::Bulge:
			gen_Wave_Base_Amplitude[i] = vec4((float)ds.deformation, (float)ds.deformationWave.func, 0, ds.bulgeHeight);
			frequency_Phase_Spread[i] = vec4(ds.bulgeSpeed, ds.bulgeWidth, 0, 0);
			break;

		case MaterialDeform::Move:
			gen_Wave_Base_Amplitude[i] = vec4((float)ds.deformation, (float)ds.deformationWave.func, ds.deformationWave.base, ds.deformationWave.amplitude);
			frequency_Phase_Spread[i] = vec4(ds.deformationWave.frequency, ds.deformationWave.phase, 0, 0);
			moveDirs[i] = ds.moveVector;
			break;

		default:
			break;
		}

		i++;
	}

	uniforms->nDeforms.set(vec4(i, 0, 0, 0));
	uniforms->deformMoveDirs.set(moveDirs, i);
	uniforms->deform_Gen_Wave_Base_Amplitude.set(gen_Wave_Base_Amplitude, i);
	uniforms->deform_Frequency_Phase_Spread.set(frequency_Phase_Spread, i);
}

} // namespace renderer
