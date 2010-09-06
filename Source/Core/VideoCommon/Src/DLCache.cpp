// Copyright (C) 2003-2009 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

// TODO: Handle cache-is-full condition :p

#include <map>

#include "Common.h"
#include "VideoCommon.h"
#include "Hash.h"
#include "MemoryUtil.h"
#include "DataReader.h"
#include "Statistics.h"
#include "OpcodeDecoding.h"   // For the GX_ constants.

#include "XFMemory.h"
#include "CPMemory.h"
#include "BPMemory.h"

#include "VertexLoaderManager.h"
#include "NativeVertexWriter.h"
#include "x64Emitter.h"
#include "ABI.h"

#include "DLCache.h"
#include "VideoConfig.h"

#define DL_CODE_CACHE_SIZE (1024*1024*16)
extern int frameCount;

using namespace Gen;

namespace DLCache
{

// Currently just recompiles the DLs themselves, doesn't bother with the vertex data.
// The speed boost is pretty small. The real big boost will come when we also store
// vertex arrays in the cached DLs.

enum DisplayListPass {
	DLPASS_ANALYZE,
	DLPASS_COMPILE,
	DLPASS_RUN,
};

struct VDataHashRegion
{
	u32 hash;
	u32 start_address;
	int size;
};
typedef u8* DataPointer;
typedef std::map<u8, DataPointer> VdataMap;

struct CachedDisplayList
{
	CachedDisplayList()
			: uncachable(false),
			pass(DLPASS_ANALYZE),
			next_check(1),
			BufferCount(0),
			num_xf_reg(0),
			num_cp_reg(0),
			num_bp_reg(0), 
			num_index_xf(0),
			num_draw_call(0)
	{
		frame_count = frameCount;
	}

	bool uncachable;  // if set, this DL will always be interpreted. This gets set if hash ever changes.

	int pass;
	u64 dl_hash;

	int check;
	int next_check;

	int frame_count;

	// ... Something containing cached vertex buffers here ...
	u8 BufferCount;	
	VdataMap Vdata;

	int num_xf_reg;
	int num_cp_reg;
	int num_bp_reg; 
	int num_index_xf;
	int num_draw_call;

	// Compile the commands themselves down to native code.
	const u8* compiled_code;
};

// We want to allow caching DLs that start at the same address but have different lengths,
// so the size has to be in the ID.
inline u64 CreateMapId(u32 address, u32 size)
{
	return ((u64)address << 32) | size;
}

typedef std::map<u64, CachedDisplayList> DLMap;

static DLMap dl_map;
static DataPointer dlcode_cache;

static Gen::XEmitter emitter;

// First pass - analyze
bool AnalyzeAndRunDisplayList(u32 address, int	 size, CachedDisplayList *dl)
{
	int num_xf_reg = 0;
	int num_cp_reg = 0;
	int num_bp_reg = 0; 
	int num_index_xf = 0;
	int num_draw_call = 0;

	u8* old_pVideoData = g_pVideoData;
	u8* startAddress = Memory_GetPtr(address);

	// Avoid the crash if Memory_GetPtr failed ..
	if (startAddress != 0)
	{
		g_pVideoData = startAddress;

		// temporarily swap dl and non-dl (small "hack" for the stats)
		Statistics::SwapDL();

		u8 *end = g_pVideoData + size;
		while (g_pVideoData < end)
		{
			// Yet another reimplementation of the DL reading...
			int cmd_byte = DataReadU8();
			switch (cmd_byte)
			{
			case GX_NOP:
				break;

			case GX_LOAD_CP_REG: //0x08
				{
					u8 sub_cmd = DataReadU8();
					u32 value = DataReadU32();
					LoadCPReg(sub_cmd, value);
					INCSTAT(stats.thisFrame.numCPLoads);
					num_cp_reg++;
				}
				break;

			case GX_LOAD_XF_REG:
				{
					u32 Cmd2 = DataReadU32();
					int transfer_size = ((Cmd2 >> 16) & 15) + 1;
					u32 xf_address = Cmd2 & 0xFFFF;
					// TODO - speed this up. pshufb?
					u32 data_buffer[16];
					for (int i = 0; i < transfer_size; i++)
						data_buffer[i] = DataReadU32();
					LoadXFReg(transfer_size, xf_address, data_buffer);
					INCSTAT(stats.thisFrame.numXFLoads);
					num_xf_reg++;
				}
				break;

			case GX_LOAD_INDX_A: //used for position matrices
				{
					LoadIndexedXF(DataReadU32(), 0xC);
					num_index_xf++;
				}
				break;
			case GX_LOAD_INDX_B: //used for normal matrices
				{
					LoadIndexedXF(DataReadU32(), 0xD);
					num_index_xf++;
				}
				break;
			case GX_LOAD_INDX_C: //used for postmatrices
				{
					LoadIndexedXF(DataReadU32(), 0xE);
					num_index_xf++;
				}
				break;
			case GX_LOAD_INDX_D: //used for lights
				{
					LoadIndexedXF(DataReadU32(), 0xF);
					num_index_xf++;
				}
				break;
			case GX_CMD_CALL_DL:
				{
					u32 addr = DataReadU32();
					u32 count = DataReadU32();
					ExecuteDisplayList(addr, count);
				}			
				break;
			case GX_CMD_UNKNOWN_METRICS: // zelda 4 swords calls it and checks the metrics registers after that
				DEBUG_LOG(VIDEO, "GX 0x44: %08x", cmd_byte);
				break;
			case GX_CMD_INVL_VC: // Invalidate Vertex Cache	
				DEBUG_LOG(VIDEO, "Invalidate (vertex cache?)");
				break;
			case GX_LOAD_BP_REG: //0x61
				{
					u32 bp_cmd = DataReadU32();
					LoadBPReg(bp_cmd);
					INCSTAT(stats.thisFrame.numBPLoads);
					num_bp_reg++;
				}
				break;

			// draw primitives 
			default:
				if (cmd_byte & 0x80)
				{
					// load vertices (use computed vertex size from FifoCommandRunnable above)
					u16 numVertices = DataReadU16();

					VertexLoaderManager::RunVertices(
						cmd_byte & GX_VAT_MASK,   // Vertex loader index (0 - 7)
						(cmd_byte & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT,
						numVertices);
					num_draw_call++;
				}
				else
				{
					ERROR_LOG(VIDEO, "OpcodeDecoding::Decode: Illegal command %02x", cmd_byte);
					break;
				}
				break;
			}
		}
		INCSTAT(stats.numDListsCalled);
		INCSTAT(stats.thisFrame.numDListsCalled);
		// un-swap
		Statistics::SwapDL();
	}
	dl->num_bp_reg = num_bp_reg;
	dl->num_cp_reg = num_cp_reg;
	dl->num_draw_call = num_draw_call;
	dl->num_index_xf = num_index_xf;
	dl->num_xf_reg = num_xf_reg;
    // reset to the old pointer
	g_pVideoData = old_pVideoData;	
	return true;
}

// The only sensible way to detect changes to vertex data is to convert several times 
// and hash the output.

// Second pass - compile
// Since some commands can affect the size of other commands, we really have no choice
// but to compile as we go, interpreting the list. We can't compile and then execute, we must
// compile AND execute at the same time. The second time the display list gets called, we already
// have the compiled code so we don't have to interpret anymore, we just run it.
bool CompileAndRunDisplayList(u32 address, int size, CachedDisplayList *dl)
{
	u8* old_pVideoData = g_pVideoData;
	u8* startAddress = Memory_GetPtr(address);

	// Avoid the crash if Memory_GetPtr failed ..
	if (startAddress != 0)
	{
		g_pVideoData = startAddress;

		// temporarily swap dl and non-dl (small "hack" for the stats)
		Statistics::SwapDL();

		u8 *end = g_pVideoData + size;

		emitter.AlignCode4();
		dl->compiled_code = emitter.GetCodePtr();
		emitter.ABI_EmitPrologue(4);

		while (g_pVideoData < end)
		{
			// Yet another reimplementation of the DL reading...
			int cmd_byte = DataReadU8();
			switch (cmd_byte)
			{
			case GX_NOP:
				// Execute
				// Compile
				break;

			case GX_LOAD_CP_REG: //0x08
				{
					// Execute
					u8 sub_cmd = DataReadU8();
					u32 value = DataReadU32();
					LoadCPReg(sub_cmd, value);
					INCSTAT(stats.thisFrame.numCPLoads);

					// Compile
					emitter.ABI_CallFunctionCC((void *)&LoadCPReg, sub_cmd, value);
				}
				break;

			case GX_LOAD_XF_REG:
				{
					// Execute
					u32 Cmd2 = DataReadU32();
					int transfer_size = ((Cmd2 >> 16) & 15) + 1;
					u32 xf_address = Cmd2 & 0xFFFF;
					// TODO - speed this up. pshufb?
					DataPointer real_data_buffer = (DataPointer) new u8[transfer_size * 4]; 
					u32 *data_buffer = (u32*)real_data_buffer;
					for (int i = 0; i < transfer_size; i++)
						data_buffer[i] = DataReadU32();
					LoadXFReg(transfer_size, xf_address, data_buffer);
					INCSTAT(stats.thisFrame.numXFLoads);
					dl->Vdata[dl->BufferCount] = real_data_buffer;
					dl->BufferCount++;
					// Compile
					emitter.ABI_CallFunctionCCP((void *)&LoadXFReg, transfer_size, xf_address, data_buffer);
				}
				break;

			case GX_LOAD_INDX_A: //used for position matrices
				{
					u32 value = DataReadU32();
					// Execute
					LoadIndexedXF(value, 0xC);
					// Compile
					emitter.ABI_CallFunctionCC((void *)&LoadIndexedXF, value, 0xC);
				}
				break;
			case GX_LOAD_INDX_B: //used for normal matrices
				{
					u32 value = DataReadU32();
					// Execute
					LoadIndexedXF(value, 0xD);
					// Compile
					emitter.ABI_CallFunctionCC((void *)&LoadIndexedXF, value, 0xD);
				}
				break;
			case GX_LOAD_INDX_C: //used for postmatrices
				{
					u32 value = DataReadU32();
					// Execute
					LoadIndexedXF(value, 0xE);
					// Compile
					emitter.ABI_CallFunctionCC((void *)&LoadIndexedXF, value, 0xE);
				}
				break;
			case GX_LOAD_INDX_D: //used for lights
				{
					u32 value = DataReadU32();
					// Execute
					LoadIndexedXF(value, 0xF);
					// Compile
					emitter.ABI_CallFunctionCC((void *)&LoadIndexedXF, value, 0xF);
				}
				break;

			case GX_CMD_CALL_DL:
				{
					u32 addr = DataReadU32();
					u32 count = DataReadU32();
					ExecuteDisplayList(addr, count);
					emitter.ABI_CallFunctionCC((void *)&ExecuteDisplayList, addr, count);
				}
				break;

			case GX_CMD_UNKNOWN_METRICS:
				// zelda 4 swords calls it and checks the metrics registers after that
				break;

			case GX_CMD_INVL_VC:// Invalidate	(vertex cache?)	
				DEBUG_LOG(VIDEO, "Invalidate	(vertex cache?)");
				break;

			case GX_LOAD_BP_REG: //0x61
				{
					u32 bp_cmd = DataReadU32();
					// Execute
					LoadBPReg(bp_cmd);
					INCSTAT(stats.thisFrame.numBPLoads);
					// Compile
					emitter.ABI_CallFunctionC((void *)&LoadBPReg, bp_cmd);
				}
				break;

				// draw primitives 
			default:
				if (cmd_byte & 0x80)
				{
					// load vertices (use computed vertex size from FifoCommandRunnable above)

					// Execute
					u16 numVertices = DataReadU16();

					u8* StartAddress = VertexManager::s_pBaseBufferPointer;
					VertexManager::Flush();
					VertexLoaderManager::RunVertices(
						cmd_byte & GX_VAT_MASK,   // Vertex loader index (0 - 7)
						(cmd_byte & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT,
						numVertices);
					u8* EndAddress = VertexManager::s_pCurBufferPointer;
					u32 Vdatasize = (u32)(EndAddress - StartAddress);
					if (size > 0)
					{
					// Compile
						DataPointer NewData = (DataPointer)new u8[Vdatasize];
						memcpy(NewData,StartAddress,Vdatasize);
						dl->Vdata[dl->BufferCount] = NewData;
						dl->BufferCount++;
						emitter.ABI_CallFunctionCCCP((void *)&VertexLoaderManager::RunCompiledVertices,
							cmd_byte & GX_VAT_MASK, (cmd_byte & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT, numVertices, NewData);	
					}
				}
				else
				{
					ERROR_LOG(VIDEO, "DLCache::CompileAndRun: Illegal command %02x", cmd_byte);
					break;
				}
				break;
			}
		}
		emitter.ABI_EmitEpilogue(4);
		INCSTAT(stats.numDListsCalled);
		INCSTAT(stats.thisFrame.numDListsCalled);
		Statistics::SwapDL();
	}
	g_pVideoData = old_pVideoData;
	return true;
}

void Init()
{
	dlcode_cache = (DataPointer)AllocateExecutableMemory(DL_CODE_CACHE_SIZE, false);  // Don't need low memory.
	emitter.SetCodePtr(dlcode_cache);
}

void Shutdown()
{
	Clear();
	FreeMemoryPages(dlcode_cache, DL_CODE_CACHE_SIZE);	
	dlcode_cache = NULL;
}

void Clear() 
{
	DLMap::iterator iter = dl_map.begin();
	while (iter != dl_map.end()) {
		CachedDisplayList &entry = iter->second;
		VdataMap::iterator viter = entry.Vdata.begin();
		while (viter != entry.Vdata.end())
		{
			DataPointer &ventry = viter->second;
			delete [] ventry;
			entry.Vdata.erase(viter++);
		}
		iter++;
	}
	dl_map.clear();
	// Reset the cache pointers.
	emitter.SetCodePtr(dlcode_cache);	
}

void ProgressiveCleanup()
{
	DLMap::iterator iter = dl_map.begin();
	while (iter != dl_map.end()) {
		CachedDisplayList &entry = iter->second;
		int limit = iter->second.uncachable ? 1200 : 400;
		if (entry.frame_count < frameCount - limit) {
			// entry.Destroy();
			VdataMap::iterator viter = entry.Vdata.begin();
			while (viter != entry.Vdata.end())
			{
				DataPointer &ventry = viter->second;
				delete [] ventry;
				entry.Vdata.erase(viter++);
			}
			dl_map.erase(iter++);  // (this is gcc standard!)
		}
		else
			++iter;
	}
}

}  // namespace

// NOTE - outside the namespace on purpose.
bool HandleDisplayList(u32 address, u32 size)
{
	//Fixed DlistCaching now is fully functional still some things to workout
	if(!g_ActiveConfig.bDlistCachingEnable)
		return false;
	if(size == 0) return false;
	u64 dl_id = DLCache::CreateMapId(address, size);
	DLCache::DLMap::iterator iter = DLCache::dl_map.find(dl_id);

	stats.numDListsAlive = (int)DLCache::dl_map.size();
	if (iter != DLCache::dl_map.end())
	{
		DLCache::CachedDisplayList &dl = iter->second;
		if (dl.uncachable)
		{
			dl.check--;
			if(dl.check <= 0)
			{
				dl.pass = DLCache::DLPASS_ANALYZE;
				dl.uncachable = false;
				dl.check = dl.next_check;
			}
			else
			{
				return false;
			}
		}

		// Got one! And it's been compiled too, so let's run the compiled code!
		switch (dl.pass)
		{
		case DLCache::DLPASS_ANALYZE:
			if (DLCache::AnalyzeAndRunDisplayList(address, size, &dl)) {
				dl.dl_hash = GetHash64(Memory_GetPtr(address), size, 0);
				dl.pass = DLCache::DLPASS_COMPILE;
				dl.check = 1;
				dl.next_check = 1;				
				return true;
			} else {
				dl.uncachable = true;			
				return true;  // don't also interpret the list.
			}
			break;
		case DLCache::DLPASS_COMPILE:
			// First, check that the hash is the same as the last time.
			if (dl.dl_hash != GetHash64(Memory_GetPtr(address), size, 0))
			{
				// PanicAlert("uncachable %08x", address);
				dl.uncachable = true;
				dl.check = 60;
				return false;
			}
			DLCache::CompileAndRunDisplayList(address, size, &dl);
			dl.pass = DLCache::DLPASS_RUN;
			break;
		case DLCache::DLPASS_RUN:
			{
				// Every N draws, check hash
				dl.check--;
				if (dl.check <= 0)
				{
					if (dl.dl_hash != GetHash64(Memory_GetPtr(address), size, 0)) 
					{
						dl.uncachable = true;
						dl.check = 60;
						DLCache::VdataMap::iterator viter = dl.Vdata.begin();
						while (viter != dl.Vdata.end())
						{
							DLCache::DataPointer &ventry = viter->second;
							delete [] ventry;
							dl.Vdata.erase(viter++);
						}
						dl.BufferCount = 0;
						return false;
					}
					dl.check = dl.next_check;
					/*dl.next_check ++;
					if (dl.next_check > 60)
						dl.next_check = 60;*/
				}
				dl.frame_count= frameCount;
				u8 *old_datareader = g_pVideoData;
				((void (*)())(void*)(dl.compiled_code))();
				Statistics::SwapDL();
				ADDSTAT(stats.thisFrame.numCPLoadsInDL, dl.num_cp_reg);
				ADDSTAT(stats.thisFrame.numXFLoadsInDL, dl.num_xf_reg);
				ADDSTAT(stats.thisFrame.numBPLoadsInDL, dl.num_bp_reg);

				ADDSTAT(stats.thisFrame.numCPLoads, dl.num_cp_reg);
				ADDSTAT(stats.thisFrame.numXFLoads, dl.num_xf_reg);
				ADDSTAT(stats.thisFrame.numBPLoads, dl.num_bp_reg);

				INCSTAT(stats.numDListsCalled);
				INCSTAT(stats.thisFrame.numDListsCalled);
				
				Statistics::SwapDL();
				g_pVideoData = old_datareader;
				break;
			}
		}
		return true;
	}

	DLCache::CachedDisplayList dl;
	
	if (DLCache::AnalyzeAndRunDisplayList(address, size, &dl)) {
		dl.dl_hash = GetHash64(Memory_GetPtr(address), size, 0);
		dl.pass = DLCache::DLPASS_COMPILE;
		dl.check = 1;
		dl.next_check = 1;
		DLCache::dl_map[dl_id] = dl;
		return true;
	} else {
		dl.uncachable = true;
		DLCache::dl_map[dl_id] = dl;
		return true;  // don't also interpret the list.
	}
}
