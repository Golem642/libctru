#include <stdlib.h>
#include <string.h>
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/gpu/registers.h>
#include <3ds/gpu/shaderProgram.h>

static void GPU_SetShaderOutmap(u32 outmapData[8]);
static void GPU_SendShaderCode(GPU_SHADER_TYPE type, u32* data, u16 offset, u16 length);
static void GPU_SendOperandDescriptors(GPU_SHADER_TYPE type, u32* data, u16 offset, u16 length);

Result shaderInstanceInit(shaderInstance_s* si, DVLE_s* dvle)
{
	if(!si || !dvle)return -1;

	si->dvle = dvle;

	si->boolUniforms = 0;
	si->boolUniformMask = 0;
	si->intUniforms[0] = 0x00000000;
	si->intUniforms[1] = 0x00000000;
	si->intUniforms[2] = 0x00000000;
	si->intUniforms[3] = 0x00000000;
	si->float24Uniforms = NULL;
	si->intUniformMask = 0;

	int i;
	DVLE_constEntry_s* cnst = dvle->constTableData;
	if(cnst)
	{
		int float24cnt=0;
		for(i=0; i<dvle->constTableSize; i++)
		{
			switch(cnst[i].type)
			{
				case DVLE_CONST_BOOL:
					shaderInstanceSetBool(si, cnst[i].id, cnst[i].data[0]&1);
					break;
				case DVLE_CONST_u8:
					if(cnst[i].id<4)
					{
						si->intUniforms[cnst[i].id] = cnst[i].data[0];
						si->intUniformMask |= (1<<cnst[i].id);
					}
					break;
				case DVLE_CONST_FLOAT24:
					float24cnt++;
					break;
			}
		}

		if(float24cnt)
		{
			si->float24Uniforms = malloc(sizeof(float24Uniform_s)*float24cnt);
			if(si->float24Uniforms)
			{
				float24cnt = 0;
				u32 rev[3];
				u8* rev8=(u8*)rev;
				for(i=0; i<dvle->constTableSize; i++)
				{
					if(cnst[i].type==DVLE_CONST_FLOAT24)
					{
						memcpy(&rev8[0], &cnst[i].data[0], 3);
						memcpy(&rev8[3], &cnst[i].data[1], 3);
						memcpy(&rev8[6], &cnst[i].data[2], 3);
						memcpy(&rev8[9], &cnst[i].data[3], 3);

						si->float24Uniforms[float24cnt].id = cnst[i].id&0xFF;
						si->float24Uniforms[float24cnt].data[0] = rev[2];
						si->float24Uniforms[float24cnt].data[1] = rev[1];
						si->float24Uniforms[float24cnt].data[2] = rev[0];

						float24cnt++;
					}
				}
			}
			si->numFloat24Uniforms = float24cnt;
		}
	}

	return 0;
}

Result shaderInstanceFree(shaderInstance_s* si)
{
	if(!si)return -1;

	if(si->float24Uniforms)free(si->float24Uniforms);
	free(si);

	return 0;
}

Result shaderInstanceSetBool(shaderInstance_s* si, int id, bool value)
{
	if(!si)return -1;
	if(id<0 || id>15)return -2;

	si->boolUniforms &= ~(1<<id);
	si->boolUniforms |= (value)<<id;
	si->boolUniformMask |= (1<<id);

	return 0;
}

Result shaderInstanceGetBool(shaderInstance_s* si, int id, bool* value)
{
	if(!si)return -1;
	if(id<0 || id>15)return -2;
	if(!value)return -3;

	*value = ((si->boolUniforms>>id)&1);

	return 0;
}

s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name)
{
	if(!si)return -1;

	return DVLE_GetUniformRegister(si->dvle, name);
}

Result shaderProgramInit(shaderProgram_s* sp)
{
	if(!sp)return -1;

	sp->vertexShader = NULL;
	sp->geometryShader = NULL;

	return 0;
}

Result shaderProgramFree(shaderProgram_s* sp)
{
	if(!sp)return -1;

	shaderInstanceFree(sp->vertexShader);
	shaderInstanceFree(sp->geometryShader);

	return 0;
}

Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle)
{
	if(!sp || !dvle)return -1;
	if(dvle->type != VERTEX_SHDR)return -2;

	if(sp->vertexShader)shaderInstanceFree(sp->vertexShader);

	sp->vertexShader = (shaderInstance_s*)malloc(sizeof(shaderInstance_s));
	if(!sp->vertexShader)return -3;

	return shaderInstanceInit(sp->vertexShader, dvle);
}

Result shaderProgramSetGsh(shaderProgram_s* sp, DVLE_s* dvle, u8 stride)
{
	if(!sp || !dvle)return -1;
	if(dvle->type != GEOMETRY_SHDR)return -2;

	if(sp->geometryShader)shaderInstanceFree(sp->geometryShader);

	sp->geometryShader = (shaderInstance_s*)malloc(sizeof(shaderInstance_s));
	if(!sp->geometryShader)return -3;

	sp->geoShaderInputPermutation[0] = 0x76543210;
	sp->geoShaderInputPermutation[1] = 0xFEDCBA98;
	sp->geoShaderInputStride = stride;

	return shaderInstanceInit(sp->geometryShader, dvle);
}

Result shaderProgramSetGshInputPermutation(shaderProgram_s* sp, u64 permutation)
{
	if(!sp || !sp->geometryShader)return -1;

	sp->geoShaderInputPermutation[0] = permutation & 0xFFFFFFFF;
	sp->geoShaderInputPermutation[0] = permutation>>32;
	return 0;
}

Result shaderProgramConfigure(shaderProgram_s* sp, bool sendVshCode, bool sendGshCode)
{
	if(!sp)return -1;

	if(!sp->vertexShader)return -2;

	// configure geostage
	// has to be done first or else VSH registers might only reconfigure 3 of the 4 shader units !
	if(!sp->geometryShader)
	{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 0x1, 0x00000000);
		GPUCMD_AddMaskedWrite(GPUREG_VSH_COM_MODE, 0x1, 0x00000000);
	}else{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 0x1, 0x00000002);
		GPUCMD_AddMaskedWrite(GPUREG_VSH_COM_MODE, 0x1, 0x00000001);
	}

	// setup vertex shader stuff no matter what
	const DVLE_s* vshDvle = sp->vertexShader->dvle;
	const DVLP_s* vshDvlp = vshDvle->dvlp;
	if (sendVshCode)
	{
		GPU_SendShaderCode(vshDvle->type, vshDvlp->codeData, 0, vshDvlp->codeSize);
		GPU_SendOperandDescriptors(vshDvle->type, vshDvlp->opcdescData, 0, vshDvlp->opdescSize);
	}
	GPUCMD_AddWrite(GPUREG_VSH_ENTRYPOINT, 0x7FFF0000|(vshDvle->mainOffset&0xFFFF));
	GPUCMD_AddWrite(GPUREG_VSH_OUTMAP_MASK, vshDvle->outmapMask);

	GPUCMD_AddWrite(GPUREG_VSH_OUTMAP_TOTAL1, vshDvle->outmapData[0]-1); // ?
	GPUCMD_AddWrite(GPUREG_VSH_OUTMAP_TOTAL2, vshDvle->outmapData[0]-1); // ?

	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 0x8, 0x00000000); // ?
	GPUCMD_AddWrite(GPUREG_GSH_MISC0, 0x00000000); // ?

	if(!sp->geometryShader)
	{
		// finish setting up vertex shader alone
		GPU_SetShaderOutmap((u32*)vshDvle->outmapData);

		GPUCMD_AddWrite(GPUREG_SH_OUTATTR_MODE, 0x00000001); // ?
		GPUCMD_AddWrite(GPUREG_SH_OUTATTR_CLOCK, 0x00000703); // ?
	}else{
		// setup both vertex and geometry shader
		const DVLE_s* gshDvle = sp->geometryShader->dvle;
		const DVLP_s* gshDvlp = gshDvle->dvlp;
		if (sendGshCode)
		{
			GPU_SendShaderCode(gshDvle->type, gshDvlp->codeData, 0, gshDvlp->codeSize);
			GPU_SendOperandDescriptors(gshDvle->type, gshDvlp->opcdescData, 0, gshDvlp->opdescSize);
		}
		GPUCMD_AddWrite(GPUREG_GSH_ENTRYPOINT, 0x7FFF0000|(gshDvle->mainOffset&0xFFFF));
		GPUCMD_AddWrite(GPUREG_GSH_OUTMAP_MASK, gshDvle->outmapMask);

		GPU_SetShaderOutmap((u32*)gshDvle->outmapData);

		//GSH input attributes stuff
		GPUCMD_AddWrite(GPUREG_GSH_INPUTBUFFER_CONFIG, 0x08000000|(sp->geoShaderInputStride-1));
		GPUCMD_AddIncrementalWrites(GPUREG_GSH_ATTRIBUTES_PERMUTATION_LOW, sp->geoShaderInputPermutation, 2);

		GPUCMD_AddWrite(GPUREG_SH_OUTATTR_MODE, 0x00000001); // ?
		GPUCMD_AddWrite(GPUREG_SH_OUTATTR_CLOCK, 0x01030703); // ?
	}

	return 0;
}

Result shaderProgramUse(shaderProgram_s* sp)
{
	Result rc = shaderProgramConfigure(sp, true, true);
	if (R_FAILED(rc)) return rc;

	int i;

	// Set up uniforms
	GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000|~sp->vertexShader->boolUniforms);
	GPUCMD_AddIncrementalWrites(GPUREG_VSH_INTUNIFORM_I0, sp->vertexShader->intUniforms, 4);
	for(i=0; i<sp->vertexShader->numFloat24Uniforms; i++) GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG, (u32*)&sp->vertexShader->float24Uniforms[i], 4);
	if (sp->geometryShader)
	{
		GPUCMD_AddWrite(GPUREG_GSH_BOOLUNIFORM, 0x7FFF0000|~sp->geometryShader->boolUniforms);
		GPUCMD_AddIncrementalWrites(GPUREG_GSH_INTUNIFORM_I0, sp->geometryShader->intUniforms, 4);
		for(i=0; i<sp->geometryShader->numFloat24Uniforms; i++) GPUCMD_AddIncrementalWrites(GPUREG_GSH_FLOATUNIFORM_CONFIG, (u32*)&sp->geometryShader->float24Uniforms[i], 4);
	}

	return 0;
}

void GPU_SetShaderOutmap(u32 outmapData[8])
{
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x1, outmapData[0]-1);
	GPUCMD_AddIncrementalWrites(GPUREG_SH_OUTMAP_TOTAL, outmapData, 8);
}

void GPU_SendShaderCode(GPU_SHADER_TYPE type, u32* data, u16 offset, u16 length)
{
	if(!data)return;

	int regOffset=(type==GPU_GEOMETRY_SHADER)?(-0x30):(0x0);

	GPUCMD_AddWrite(GPUREG_VSH_CODETRANSFER_CONFIG+regOffset, offset);

	int i;
	for(i=0;i<length;i+=0x80)GPUCMD_AddWrites(GPUREG_VSH_CODETRANSFER_DATA+regOffset, &data[i], ((length-i)<0x80)?(length-i):0x80);

	GPUCMD_AddWrite(GPUREG_VSH_CODETRANSFER_END+regOffset, 0x00000001);
}

void GPU_SendOperandDescriptors(GPU_SHADER_TYPE type, u32* data, u16 offset, u16 length)
{
	if(!data)return;

	int regOffset=(type==GPU_GEOMETRY_SHADER)?(-0x30):(0x0);

	GPUCMD_AddWrite(GPUREG_VSH_OPDESCS_CONFIG+regOffset, offset);

	GPUCMD_AddWrites(GPUREG_VSH_OPDESCS_DATA+regOffset, data, length);
}
