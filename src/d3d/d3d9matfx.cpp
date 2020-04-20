#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwplugins.h"
#include "rwd3d.h"
#include "rwd3d9.h"

namespace rw {
namespace d3d9 {
using namespace d3d;

static void *matfx_env_amb_VS;
static void *matfx_env_amb_dir_VS;
static void *matfx_env_all_VS;
static void *matfx_env_PS;
static void *matfx_env_tex_PS;

enum
{
	VSLOC_texMat = VSLOC_afterLights,

	PSLOC_shininess = 1,
	PSLOC_colorClamp = 2
};

void
matfxRender_Default(InstanceDataHeader *header, InstanceData *inst, int32 lightBits)
{
	Material *m = inst->material;

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(default_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(default_amb_dir_VS);
	else
		setVertexShader(default_all_VS);

	SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 255);

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(default_tex_PS);
	}else
		setPixelShader(default_PS);

	drawInst(header, inst);
}

static Frame *lastEnvFrame;

static RawMatrix normal2texcoord = {
	{ 0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

void
uploadEnvMatrix(Frame *frame)
{
	Matrix invMat;
	if(frame == nil)
		frame = engine->currentCamera->getFrame();

	// cache the matrix across multiple meshes
	if(frame == lastEnvFrame)
		return;
	lastEnvFrame = frame;

	RawMatrix envMtx, invMtx;
	Matrix::invert(&invMat, frame->getLTM());
	convMatrix(&invMtx, &invMat);
	RawMatrix::mult(&envMtx, &invMtx, &normal2texcoord);
	d3ddevice->SetVertexShaderConstantF(VSLOC_texMat, (float*)&envMtx, 4);
}

void
matfxRender_EnvMap(InstanceDataHeader *header, InstanceData *inst, int32 lightBits, MatFX::Env *env)
{
	Material *m = inst->material;

	if(env->tex == nil || env->coefficient == 0.0f){
		matfxRender_Default(header, inst, lightBits);
		return;
	}

	d3d::setTexture(1, env->tex);
	uploadEnvMatrix(env->frame);

	d3ddevice->SetPixelShaderConstantF(PSLOC_shininess, &env->coefficient, 1);

	SetRenderState(SRCBLEND, BLENDONE);

	static float zero[4];
	static float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	d3ddevice->SetPixelShaderConstantF(PSLOC_shininess, &env->coefficient, 1);
	// This clamps the vertex color below. With it we can achieve both PC and PS2 style matfx
	if(MatFX::modulateEnvMap)
		d3ddevice->SetPixelShaderConstantF(PSLOC_colorClamp, zero, 1);
	else
		d3ddevice->SetPixelShaderConstantF(PSLOC_colorClamp, one, 1);

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(matfx_env_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(matfx_env_amb_dir_VS);
	else
		setVertexShader(matfx_env_all_VS);

	bool32 texAlpha = PLUGINOFFSET(D3dRaster, env->tex->raster, nativeRasterOffset)->hasAlpha;

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(matfx_env_tex_PS);
	}else
		setPixelShader(matfx_env_PS);

	SetRenderState(VERTEXALPHA, texAlpha || inst->vertexAlpha || m->color.alpha != 255);

	drawInst(header, inst);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header)
{
	int vsBits;
	d3ddevice->SetStreamSource(0, (IDirect3DVertexBuffer9*)header->vertexStream[0].vertexBuffer,
	                           0, header->vertexStream[0].stride);
	d3ddevice->SetIndices((IDirect3DIndexBuffer9*)header->indexBuffer);
	d3ddevice->SetVertexDeclaration((IDirect3DVertexDeclaration9*)header->vertexDeclaration);

	lastEnvFrame = nil;

	vsBits = lightingCB_Shader(atomic);
	uploadMatrices(atomic->getFrame()->getLTM());

	d3ddevice->SetVertexShaderConstantF(VSLOC_fogData, (float*)&d3dShaderState.fogData, 1);
	d3ddevice->SetPixelShaderConstantF(PSLOC_fogColor, (float*)&d3dShaderState.fogColor, 1);

	bool normals = !!(atomic->geometry->flags & Geometry::NORMALS);

	float surfProps[4];
	surfProps[3] = atomic->geometry->flags&Geometry::PRELIT ? 1.0f : 0.0f;

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		rw::RGBAf col;
		convColor(&col, &inst->material->color);
		d3ddevice->SetVertexShaderConstantF(VSLOC_matColor, (float*)&col, 1);

		surfProps[0] = m->surfaceProps.ambient;
		surfProps[1] = m->surfaceProps.specular;
		surfProps[2] = m->surfaceProps.diffuse;
		d3ddevice->SetVertexShaderConstantF(VSLOC_surfProps, surfProps, 1);

		MatFX *matfx = MatFX::get(m);
		if(matfx == nil)
			matfxRender_Default(header, inst, vsBits);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			if(normals)
				matfxRender_EnvMap(header, inst, vsBits, &matfx->fx[0].env);
			else
				matfxRender_Default(header, inst, vsBits);
			break;
		case MatFX::NOTHING:
		case MatFX::BUMPMAP:
		case MatFX::BUMPENVMAP:
		case MatFX::DUAL:
		case MatFX::UVTRANSFORM:
		case MatFX::DUALUVTRANSFORM:
			// not supported yet
			matfxRender_Default(header, inst, vsBits);
			break;			
		}

		inst++;
	}

	d3d::setTexture(1, nil);

	setVertexShader(nil);
	setPixelShader(nil);
}

#define VS_NAME g_vs20_main
#define PS_NAME g_ps20_main

void
createMatFXShaders(void)
{
	{
		static
#include "shaders/matfx_env_amb_VS.h"
		matfx_env_amb_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_amb_VS);
	}
	{
		static
#include "shaders/matfx_env_amb_dir_VS.h"
		matfx_env_amb_dir_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_amb_dir_VS);
	}
	{
		static
#include "shaders/matfx_env_all_VS.h"
		matfx_env_all_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_all_VS);
	}


	{
		static
#include "shaders/matfx_env_PS.h"
		matfx_env_PS = createPixelShader((void*)PS_NAME);
		assert(matfx_env_PS);
	}
	{
		static
#include "shaders/matfx_env_tex_PS.h"
		matfx_env_tex_PS = createPixelShader((void*)PS_NAME);
		assert(matfx_env_tex_PS);
	}

}

void
destroyMatFXShaders(void)
{
	destroyVertexShader(matfx_env_PS);
	matfx_env_PS = nil;

	destroyVertexShader(matfx_env_tex_PS);
	matfx_env_tex_PS = nil;
}

static void*
matfxOpen(void *o, int32, int32)
{
	createMatFXShaders();

	matFXGlobals.pipelines[PLATFORM_D3D9] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	destroyMatFXShaders();

	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_D3D9, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = new ObjPipeline(PLATFORM_D3D9);
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB_Shader;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

}
}
