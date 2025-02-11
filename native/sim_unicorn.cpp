#define __STDC_FORMAT_MACROS 1

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <queue>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libvex.h>
#include <pyvex.h>
}

#include <unicorn/unicorn.h>

#include "sim_unicorn.hpp"
//#include "log.h"

State::State(uc_engine *_uc, uint64_t cache_key):uc(_uc) {
	hooked = false;
	h_read = h_write = h_block = h_prot = 0;
	max_steps = cur_steps = 0;
	stopped = true;
	stop_details.stop_reason = STOP_NOSTART;
	ignore_next_block = false;
	ignore_next_selfmod = false;
	interrupt_handled = false;
	transmit_sysno = -1;
	vex_guest = VexArch_INVALID;
	syscall_count = 0;
	uc_context_alloc(uc, &saved_regs);
	executed_pages_iterator = NULL;
	cpu_flags_register = -1;
	mem_updates_head = NULL;

	auto it = global_cache.find(cache_key);
	if (it == global_cache.end()) {
		page_cache = new PageCache();
		global_cache[cache_key] = {page_cache};
	} else {
		page_cache = it->second.page_cache;
	}
	arch = *((uc_arch*)uc); // unicorn hides all its internals...
	mode = *((uc_mode*)((uc_arch*)uc + 1));
	curr_block_details.reset();
}

/*
 * HOOK_MEM_WRITE is called before checking if the address is valid. so we might
 * see uninitialized pages. Using HOOK_MEM_PROT is too late for tracking taint.
 * so we don't have to use HOOK_MEM_PROT to track dirty pages.
 */
void State::hook() {
	if (hooked) {
		//LOG_D("already hooked");
		return ;
	}
	uc_err err;
	err = uc_hook_add(uc, &h_read, UC_HOOK_MEM_READ, (void *)hook_mem_read, this, 1, 0);

	err = uc_hook_add(uc, &h_write, UC_HOOK_MEM_WRITE, (void *)hook_mem_write, this, 1, 0);

	err = uc_hook_add(uc, &h_block, UC_HOOK_BLOCK, (void *)hook_block, this, 1, 0);

	err = uc_hook_add(uc, &h_prot, UC_HOOK_MEM_PROT, (void *)hook_mem_prot, this, 1, 0);

	err = uc_hook_add(uc, &h_unmap, UC_HOOK_MEM_UNMAPPED, (void *)hook_mem_unmapped, this, 1, 0);

	err = uc_hook_add(uc, &h_intr, UC_HOOK_INTR, (void *)hook_intr, this, 1, 0);

	hooked = true;
}

void State::unhook() {
	if (!hooked)
		return ;

	uc_err err;
	err = uc_hook_del(uc, h_read);
	err = uc_hook_del(uc, h_write);
	err = uc_hook_del(uc, h_block);
	err = uc_hook_del(uc, h_prot);
	err = uc_hook_del(uc, h_unmap);
	err = uc_hook_del(uc, h_intr);

	hooked = false;
	h_read = h_write = h_block = h_prot = h_unmap = 0;
}

uc_err State::start(address_t pc, uint64_t step) {
	stopped = false;
	stop_details.stop_reason = STOP_NOSTART;
	max_steps = step;
	cur_steps = -1;
	unicorn_next_instr_addr = pc;
	executed_pages.clear();

	// error if pc is 0
	// TODO: why is this check here and not elsewhere
	if (pc == 0) {
		stop_details.stop_reason = STOP_ZEROPAGE;
		cur_steps = 0;
		return UC_ERR_MAP;
	}

	// Initialize slice tracker for registers and flags
	for (auto &reg_entry: vex_to_unicorn_map) {
		reg_instr_slice.emplace(reg_entry.first, std::vector<instr_details_t>());
	}

	for (auto &cpu_flag_entry: cpu_flags) {
		reg_instr_slice.emplace(cpu_flag_entry.first, std::vector<instr_details_t>());
	}

	uc_err out = uc_emu_start(uc, unicorn_next_instr_addr, 0, 0, 0);
	if (out == UC_ERR_OK && stop_details.stop_reason == STOP_NOSTART && get_instruction_pointer() == 0) {
		// handle edge case where we stop because we reached our bogus stop address (0)
		commit();
		stop_details.stop_reason = STOP_ZEROPAGE;
	}
	rollback();

	if (out == UC_ERR_INSN_INVALID) {
		stop_details.stop_reason = STOP_NODECODE;
	}

	// if we errored out right away, fix the step count to 0
	if (cur_steps == -1) cur_steps = 0;

	return out;
}

void State::stop(stop_t reason, bool do_commit) {
	stopped = true;
	stop_details.stop_reason = reason;
	stop_details.block_addr = curr_block_details.block_addr;
	stop_details.block_size = curr_block_details.block_size;
	if ((reason == STOP_SYSCALL) || do_commit) {
		commit();
	}
	else if ((reason != STOP_NORMAL) && (reason != STOP_STOPPOINT)) {
		// Stop reason is not NORMAL, STOPPOINT or SYSCALL. Rollback.
		// EXECNONE, ZEROPAGE, NOSTART, ZERO_DIV, NODECODE and HLT are never passed to this function.
		rollback();
	}
	uc_emu_stop(uc);
	// Prepare details of blocks with symbolic instructions to re-execute for returning to state plugin
	for (auto &block: blocks_with_symbolic_instrs) {
		sym_block_details_t sym_block;
		sym_block.reset();
		sym_block.block_addr = block.block_addr;
		sym_block.block_size = block.block_size;
		std::set<instr_details_t> sym_instrs;
		std::unordered_set<register_value_t> reg_values;
		for (auto &sym_instr: block.symbolic_instrs) {
			auto sym_instr_list = get_list_of_dep_instrs(sym_instr);
			sym_instrs.insert(sym_instr_list.begin(), sym_instr_list.end());
			sym_instrs.insert(sym_instr);
			reg_values.insert(sym_instr.reg_deps.begin(), sym_instr.reg_deps.end());
		}
		sym_block.register_values.insert(sym_block.register_values.end(), reg_values.begin(), reg_values.end());
		for (auto &instr: sym_instrs) {
			sym_instr_details_t sym_instr;
			sym_instr.instr_addr = instr.instr_addr;
			sym_instr.memory_values = instr.memory_values;
			sym_instr.memory_values_count = instr.memory_values_count;
			sym_instr.has_memory_dep = instr.has_concrete_memory_dep;
			sym_block.symbolic_instrs.emplace_back(sym_instr);
		}
		block_details_to_return.emplace_back(sym_block);
	}
}

void State::step(address_t current_address, int32_t size, bool check_stop_points) {
	if (track_bbls) {
		bbl_addrs.push_back(current_address);
	}
	if (track_stack) {
		stack_pointers.push_back(get_stack_pointer());
	}
	executed_pages.insert(current_address & ~0xFFFULL);
	cur_address = current_address;
	cur_size = size;

	if (cur_steps >= max_steps) {
		stop(STOP_NORMAL);
	} else if (check_stop_points) {
		// If size is zero, that means that the current basic block was too large for qemu
		// and it got split into multiple parts. unicorn will only call this hook for the
		// first part and not for the remaining ones, so it is impossible to find the
		// accurate size of the BB block here.
		//
		// See https://github.com/unicorn-engine/unicorn/issues/874
		//
		// Until that is resolved, we use the maximum size of a Qemu basic block here. This means
		// that some stop points may not work, but there is no way to do better.
		uint32_t real_size = size == 0 ? MAX_BB_SIZE : size;

		// if there are any stop points in the current basic block, then there is no chance
		// for us to stop in the middle of a block.
		// since we do not support stopping in the middle of a block.

		auto stop_point = stop_points.lower_bound(current_address);
		if (stop_point != stop_points.end() && *stop_point < current_address + real_size) {
			stop(STOP_STOPPOINT);
		}
	}
}

void State::commit() {
	// save registers
	uc_context_save(uc, saved_regs);

	// mark memory sync status
	// we might miss some dirty bits, this happens if hitting the memory
	// write before mapping
	/*  THIS SHOULDN'T BE REQUIRED, we have the same logic to do this in the page activation code
	for (auto it = mem_writes.begin(); it != mem_writes.end(); it++) {
		if (it->clean == -1) {
			taint_t *bitmap = page_lookup(it->address);
			if (it->is_symbolic) {
				memset(&bitmap[it->address & 0xFFFULL], TAINT_SYMBOLIC, sizeof(taint_t) * it->size);
			}
			else {
				memset(&bitmap[it->address & 0xFFFULL], TAINT_DIRTY, sizeof(taint_t) * it->size);
				it->clean = (1 << it->size) - 1;
				//LOG_D("commit: lazy initialize mem_write [%#lx, %#lx]", it->address, it->address + it->size);
			}
		}
	}
	*/

	// clear memory rollback status
	mem_writes.clear();
	cur_steps++;

	// Sync all block level taint statuses reads with state's taint statuses and block level
	// symbolic instruction list with state's symbolic instruction list
	for (auto &reg_offset: block_symbolic_registers) {
		symbolic_registers.emplace(reg_offset);
	}
	for (auto &reg_offset: block_concrete_registers) {
		symbolic_registers.erase(reg_offset);
	}
	if (curr_block_details.symbolic_instrs.size() > 0) {
		for (auto &symbolic_instr: curr_block_details.symbolic_instrs) {
			// Save all concrete memory dependencies of the block
			save_concrete_memory_deps(symbolic_instr);
		}
		blocks_with_symbolic_instrs.emplace_back(curr_block_details);
	}
	// Clear all block level taint status trackers and symbolic instruction list
	block_symbolic_registers.clear();
	block_concrete_registers.clear();
	curr_block_details.reset();
	instr_slice_details_map.clear();
	mem_reads_map.clear();
	mem_writes_taint_map.clear();
	return;
}

void State::rollback() {
	// roll back memory changes
	for (auto rit = mem_writes.rbegin(); rit != mem_writes.rend(); rit++) {
		uc_err err = uc_mem_write(uc, rit->address, rit->value, rit->size);
		if (err) {
			//LOG_I("rollback: %s", uc_strerror(err));
			break;
		}
		auto page = page_lookup(rit->address);
		taint_t *bitmap = page.first;

		uint64_t start = rit->address & 0xFFF;
		int size = rit->size;
		for (auto i = 0; i < size; i++) {
			bitmap[start + i] = rit->previous_taint[i];
		}
	}
	mem_writes.clear();

	// restore registers
	uc_context_restore(uc, saved_regs);

	if (track_bbls) bbl_addrs.pop_back();
	if (track_stack) stack_pointers.pop_back();
}

/*
 * return the PageBitmap only if the page is remapped for writing,
 * or initialized with symbolic variable, otherwise return NULL.
 */
std::pair<taint_t *, uint8_t *> State::page_lookup(address_t address) const {
	address &= ~0xFFFULL;
	auto it = active_pages.find(address);
	if (it == active_pages.end()) {
		return std::pair<taint_t *, uint8_t *>(NULL, NULL);
	}
	return it->second;
}

void State::page_activate(address_t address, uint8_t *taint, uint8_t *data) {
	address &= ~0xFFFULL;
	auto it = active_pages.find(address);
	if (it == active_pages.end()) {
		if (data == NULL) {
			// We need to copy the taint bitmap
			taint_t *bitmap = new PageBitmap;
			memcpy(bitmap, taint, sizeof(PageBitmap));

			active_pages.insert(std::pair<address_t, std::pair<taint_t*, uint8_t*>>(address, std::pair<taint_t*, uint8_t*>(bitmap, NULL)));
		} else {
			// We can directly use the passed taint and data
			taint_t *bitmap = (taint_t*)taint;
			active_pages.insert(std::pair<uint64_t, std::pair<taint_t*, uint8_t*>>(address, std::pair<taint_t*, uint8_t*>(bitmap, data)));
		}
	} else {
		// TODO: un-hardcode this address, or at least do this warning from python land
		if (address == 0x4000) {
			printf("[sim_unicorn] You've mapped something at 0x4000! "
				"Please don't do that, I put my GDT there!\n");
		} else {
			printf("[sim_unicorn] Something very bad is happening; please investigate. "
				"Trying to activate the page at %#" PRIx64 " but it's already activated.\n", address);
			// to the person who sees this error:
			// you're gonna need to spend some time looking into it.
			// I'm not 100% sure that this is necessarily a bug condition.
		}
	}

	/*  SHOULD NOT BE NECESSARY
	for (auto a = mem_writes.begin(); a != mem_writes.end(); a++)
		if (a->clean == -1 && (a->address & ~0xFFFULL) == address) {
			// This mapping was prompted by this write.
			// initialize this memory access immediately so that the
			// record is valid.
			//printf("page_activate: lazy initialize mem_write [%#lx, %#lx]\n", a->address, a->address + a->size);
			if (data == NULL) {
				memset(&bitmap[a->address & 0xFFFULL], TAINT_DIRTY, sizeof(taint_t) * a->size);
				a->clean = (1ULL << a->size) - 1;
			} else {
				a->clean = 0;
				for (int i = 0; i < a->size; i++) {
					if (bitmap[(a->address & 0xFFFULL) + i] == TAINT_SYMBOLIC) {
						a->clean |= 1 << i;
					}
				}
				memset(&bitmap[a->address & 0xFFFULL], TAINT_CLEAN, sizeof(taint_t) * a->size);
				memcpy(a->value, &data[a->address & 0xFFFULL], a->size);
			}
		}
	*/
}

mem_update_t *State::sync() {
	for (auto it = active_pages.begin(); it != active_pages.end(); it++) {
		uint8_t *data = it->second.second;
		if (data != NULL) {
			// nothing to sync, direct mapped :)
			continue;
		}
		taint_t *start = it->second.first;
		taint_t *end = &it->second.first[0x1000];
		//LOG_D("found active page %#lx (%p)", it->first, start);
		for (taint_t *i = start; i < end; i++)
			if ((*i) == TAINT_DIRTY) {
				taint_t *j = i;
				while (j < end && (*j) == TAINT_DIRTY) j++;

				char buf[0x1000];
				uc_mem_read(uc, it->first + (i - start), buf, j - i);
				//LOG_D("sync [%#lx, %#lx] = %#lx", it->first + (i - start), it->first + (j - start), *(uint64_t *)buf);

				mem_update_t *range = new mem_update_t;
				range->address = it->first + (i - start);
				range->length = j - i;
				range->next = mem_updates_head;
				mem_updates_head = range;

				i = j;
			}
	}

	return mem_updates_head;
}

void State::set_stops(uint64_t count, address_t *stops) {
	stop_points.clear();
	for (auto i = 0; i < count; i++) {
		stop_points.insert(stops[i]);
	}
}

std::pair<address_t, size_t> State::cache_page(address_t address, size_t size, char* bytes, uint64_t permissions) {
	assert(address % 0x1000 == 0);
	assert(size % 0x1000 == 0);

	for (auto offset = 0; offset < size; offset += 0x1000) {
		auto page = page_cache->find(address+offset);
		if (page != page_cache->end()) {
			fprintf(stderr, "[%#" PRIx64 ", %#" PRIx64 "](%#zx) already in cache.\n", address+offset, address+offset + 0x1000, 0x1000lu);
			assert(page->second.size == 0x1000);
			assert(memcmp(page->second.bytes, bytes + offset, 0x1000) == 0);

			continue;
		}

		uint8_t *copy = (uint8_t *)malloc(0x1000);
		CachedPage cached_page = {
			0x1000,
			copy,
			permissions
		};
		// address should be aligned to 0x1000
		memcpy(copy, &bytes[offset], 0x1000);
		page_cache->insert(std::pair<address_t, CachedPage>(address+offset, cached_page));
	}
	return std::make_pair(address, size);
}

void State::wipe_page_from_cache(address_t address) {
	auto page = page_cache->find(address);
	if (page != page_cache->end()) {
		//printf("Internal: unmapping %#llx size %#x, result %#x", page->first, page->second.size, uc_mem_unmap(uc, page->first, page->second.size));
		uc_err err = uc_mem_unmap(uc, page->first, page->second.size);
		//if (err) {
		//	fprintf(stderr, "wipe_page_from_cache [%#lx, %#lx]: %s\n", page->first, page->first + page->second.size, uc_strerror(err));
		//}
		free(page->second.bytes); // might explode
		page_cache->erase(page);
	}
	else {
		//printf("Uh oh! Couldn't find page at %#llx\n", address);
	}
}

void State::uncache_pages_touching_region(address_t address, uint64_t length) {
	address &= ~(0x1000-1);
	for (auto offset = 0; offset < length; offset += 0x1000) {
				wipe_page_from_cache(address + offset);
	}

}

void State::clear_page_cache() {
	while (!page_cache->empty()) {
		wipe_page_from_cache(page_cache->begin()->first);
	}
}

bool State::map_cache(address_t address, size_t size) {
	assert(address % 0x1000 == 0);
	assert(size % 0x1000 == 0);

	bool success = true;

	for (auto offset = 0; offset < size; offset += 0x1000) {
		auto page = page_cache->find(address+offset);
		if (page == page_cache->end())
		{
			success = false;
			continue;
		}

		auto cached_page = page->second;
		size_t page_size = cached_page.size;
		uint8_t *bytes = cached_page.bytes;
		uint64_t permissions = cached_page.perms;

		assert(page_size == 0x1000);

		//LOG_D("hit cache [%#lx, %#lx]", address, address + size);
		uc_err err = uc_mem_map_ptr(uc, page->first, page_size, permissions, bytes);
		if (err) {
			fprintf(stderr, "map_cache [%#lx, %#lx]: %s\n", address, address + size, uc_strerror(err));
			success = false;
			continue;
		}
	}
	return success;
}

bool State::in_cache(address_t address) const {
	return page_cache->find(address) != page_cache->end();
}

// Finds tainted data in the provided range and returns the address.
// Returns -1 if no tainted data is present.
int64_t State::find_tainted(address_t address, int size) {
	taint_t *bitmap = page_lookup(address).first;

	int start = address & 0xFFF;
	int end = (address + size - 1) & 0xFFF;

	if (end >= start) {
		if (bitmap) {
			for (auto i = start; i <= end; i++) {
				if (bitmap[i] & TAINT_SYMBOLIC) {
					return (address & ~0xFFF) + i;
				}
			}
		}
	}
	else {
		// cross page boundary
		if (bitmap) {
			for (auto i = start; i <= 0xFFF; i++) {
				if (bitmap[i] & TAINT_SYMBOLIC) {
					return (address & ~0xFFF) + i;
				}
			}
		}

		bitmap = page_lookup(address + size - 1).first;
		if (bitmap) {
			for (auto i = 0; i <= end; i++) {
				if (bitmap[i] & TAINT_SYMBOLIC) {
					return ((address + size - 1) & ~0xFFF) + i;
				}
			}
		}
	}
	return -1;
}

void State::handle_write(address_t address, int size, bool is_interrupt) {
	// If the write spans a page, chop it up
	if ((address & 0xfff) + size > 0x1000) {
		int chopsize = 0x1000 - (address & 0xfff);
		handle_write(address, chopsize, is_interrupt);
		if (stopped) {
			return;
		}
		handle_write(address + chopsize, size - chopsize, is_interrupt);
		return;
	}

	// From here, we are definitely only dealing with one page

	mem_write_t record;
	record.address = address;
	record.size = size;
	uc_err err = uc_mem_read(uc, address, record.value, size);
	if (err == UC_ERR_READ_UNMAPPED) {
		if (py_mem_callback(uc, UC_MEM_WRITE_UNMAPPED, address, size, 0, (void*)1)) {
			err = UC_ERR_OK;
		}
	}
	if (err) {
		stop(STOP_ERROR);
		return;
	}

	auto pair = page_lookup(address);
	taint_t *bitmap = pair.first;
	uint8_t *data = pair.second;
	int start = address & 0xFFF;
	int end = (address + size - 1) & 0xFFF;
	short clean;
	address_t curr_instr_addr;
	bool is_dst_symbolic;

	if (!bitmap) {
		// We should never have a missing bitmap because we explicitly called the callback!
		printf("This should never happen, right? %#" PRIx64 "\n", address);
		abort();
	}

	clean = 0;
	if (is_interrupt || is_symbolic_tracking_disabled() || curr_block_details.vex_lift_failed) {
		// If symbolic tracking is disabled, all writes are concrete
		// is_interrupt flag is a workaround for CGC transmit syscall, which never passes symbolic data
		// If VEX lift failed, then write is definitely concrete since execution continues only
		// if no symbolic data is present
		is_dst_symbolic = false;
	}
	else {
		curr_instr_addr = get_instruction_pointer();
		auto mem_writes_taint_map_entry = mem_writes_taint_map.find(curr_instr_addr);
		if (mem_writes_taint_map_entry != mem_writes_taint_map.end()) {
			is_dst_symbolic = mem_writes_taint_map_entry->second;
		}
		// We did not find a memory write at this instruction when processing the VEX statements.
		// This likely means unicorn reported the current PC register value wrong.
		// If there are no symbolic registers, assume write is concrete and continue concrete execution else stop.
		else if ((symbolic_registers.size() == 0) && (block_symbolic_registers.size() == 0)) {
			is_dst_symbolic = false;
		}
		else {
			stop(STOP_UNKNOWN_MEMORY_WRITE);
			return;
		}
	}
	if (is_dst_symbolic) {
		// Save the details of memory location written to in the instruction details
		for (auto &symbolic_instr: curr_block_details.symbolic_instrs) {
			if (symbolic_instr.instr_addr == curr_instr_addr) {
				symbolic_instr.mem_write_addr = address;
				symbolic_instr.mem_write_size = size;
				break;
			}
		}
	}
	if ((find_tainted(address, size) != -1) & (!is_dst_symbolic)) {
		// We are writing to a memory location that is currently symbolic. If the destination if a memory dependency
		// of some instruction to be re-executed, we need to re-execute that instruction before continuing.
		auto write_start_addr = address;
		auto write_end_addr = address + size;
		for (auto &block: blocks_with_symbolic_instrs) {
			for (auto &sym_instr: block.symbolic_instrs) {
				for (auto &symbolic_mem_dep: sym_instr.symbolic_mem_deps) {
					auto symbolic_start_addr = symbolic_mem_dep.first;
					auto symbolic_end_addr = symbolic_mem_dep.first + symbolic_mem_dep.second;
					if (!((symbolic_end_addr < write_start_addr) || (write_end_addr < symbolic_start_addr))) {
						// No overlap condition test failed => there is some overlap. Thus, some symbolic memory dependency
						// will be lost. Stop execution.
						stop(STOP_SYMBOLIC_MEM_DEP_NOT_LIVE);
						return;
					}
				}
			}
		}
		// Also check if the destination is a memory dependency of an instruction in current block should be re-executed
		for (auto &sym_instr: curr_block_details.symbolic_instrs) {
			for (auto &symbolic_mem_dep: sym_instr.symbolic_mem_deps) {
				auto symbolic_start_addr = symbolic_mem_dep.first;
				auto symbolic_end_addr = symbolic_mem_dep.first + symbolic_mem_dep.second;
				if (!((symbolic_end_addr < write_start_addr) || (write_end_addr < symbolic_start_addr))) {
					// No overlap condition test failed => there is some overlap. Thus, some symbolic memory dependency
					// will be lost. Stop execution.
					stop(STOP_SYMBOLIC_MEM_DEP_NOT_LIVE_CURR_BLOCK);
					return;
				}
			}
		}
		// The destination is not a memory dependency of some instruction to be re-executed. We now check if any
		// instructions to be re-executed write to this same location. If there is one, they need not be re-executed
		// since this concrete write nullifies their effects.
		auto curr_write_start_addr = address;
		auto curr_write_end_addr = address + size;
		std::vector<std::vector<instr_details_t>::iterator> instrs_to_erase_it;
		for (auto &block: blocks_with_symbolic_instrs) {
			instrs_to_erase_it.clear();
			for (auto sym_instr_it = block.symbolic_instrs.begin(); sym_instr_it != block.symbolic_instrs.end(); sym_instr_it++) {
				int64_t symbolic_write_start_addr = sym_instr_it->mem_write_addr;
				if (symbolic_write_start_addr == -1) {
					// Instruction does not write a symbolic write to memory. No need to check this.
					continue;
				}
				int64_t symbolic_write_end_addr = sym_instr_it->mem_write_addr + sym_instr_it->mem_write_size;
				if ((curr_write_start_addr <= symbolic_write_start_addr) && (symbolic_write_end_addr <= curr_write_end_addr)) {
					// Currrent write fully overwrites the previous written symbolic value and so the symbolic write
					// instruction need not be re-executed
					// TODO: How to handle partial overwrite?
					// TODO: If this block is not fully executed in unicorn before control returns to python land,
					// the state will be inconsistent until this concrete memory write is executed. If this happens,
					// the symbolic memory write should be removed from list of instructions to re-execute in commit.
					instrs_to_erase_it.emplace_back(sym_instr_it);
				}
			}
			for (auto &instr_to_erase_it: instrs_to_erase_it) {
				block.symbolic_instrs.erase(instr_to_erase_it);
			}
		}
	}
	if (data == NULL) {
		for (auto i = start; i <= end; i++) {
			record.previous_taint.push_back(bitmap[i]);
			if (is_dst_symbolic) {
				// Don't mark as TAINT_DIRTY since we don't want to sync it back to angr
				// Also, no need to set clean: rollback will set it to TAINT_NONE which
				// is fine for symbolic bytes and rollback is called when exiting unicorn
				// due to an error encountered
				bitmap[i] = TAINT_SYMBOLIC;
			}
			else if (bitmap[i] != TAINT_DIRTY) {
				bitmap[i] = TAINT_DIRTY;
			}
		}
	}
	else {
		for (auto i = start; i <= end; i++) {
			record.previous_taint.push_back(bitmap[i]);
			if (is_dst_symbolic) {
				// Don't mark as TAINT_DIRTY since we don't want to sync it back to angr
				// Also, no need to set clean: rollback will set it to TAINT_NONE which
				// is fine for symbolic bytes and rollback is called when exiting unicorn
				// due to an error encountered
				bitmap[i] = TAINT_SYMBOLIC;
			}
			else if (bitmap[i] != TAINT_NONE) {
				bitmap[i] = TAINT_NONE;
			}
		}
	}
	mem_writes.push_back(record);
}

void State::compute_slice_of_instrs(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry) {
	instr_slice_details_t instr_slice_details;
	for (auto &dependency: instr_taint_entry.dependencies.at(TAINT_ENTITY_REG)) {
		auto dep_reg_slice_entry = reg_instr_slice.find(dependency.reg_offset);
		if (dep_reg_slice_entry == reg_instr_slice.end()) {
			// We don't care about slice of this register because it's artificial, blacklisted or something else.
			continue;
		}
		if (!is_symbolic_register(dependency.reg_offset, dependency.value_size)) {
			auto dep_reg_slice_instrs = dep_reg_slice_entry->second;
			if (dep_reg_slice_instrs.size() == 0) {
				// The register was not modified in this block by any preceding instruction
				// and so it's value at start of block is a dependency of the block
				instr_slice_details.concrete_registers.emplace(dependency.reg_offset, dependency.value_size);
			}
			else {
				// The register was modified by some instructions in the block. We add those
				// instructions to the slice of this instruction and also any instructions
				// they depend on
				for (auto &dep_reg_slice_instr: dep_reg_slice_instrs) {
					auto& dep_instr_slice_details = instr_slice_details_map.at(dep_reg_slice_instr.instr_addr);
					instr_slice_details.concrete_registers.insert(dep_instr_slice_details.concrete_registers.begin(), dep_instr_slice_details.concrete_registers.end());
					instr_slice_details.dependent_instrs.insert(dep_instr_slice_details.dependent_instrs.begin(), dep_instr_slice_details.dependent_instrs.end());
					instr_slice_details.dependent_instrs.emplace(dep_reg_slice_instr);
				}
			}
		}
	}
	std::queue<std::pair<taint_entity_t, address_t>> temps_to_process;
	auto &curr_block_taint_entry = block_taint_cache.at(curr_block_details.block_addr);
	for (auto &dependency: instr_taint_entry.dependencies.at(TAINT_ENTITY_TMP)) {
		auto vex_temp_deps_entry = curr_block_taint_entry.vex_temp_deps.find(dependency);
		if (vex_temp_deps_entry == curr_block_taint_entry.vex_temp_deps.end()) {
			// No dependency entries for this VEX temp
			continue;
		}
		address_t vex_setter_instr = vex_temp_deps_entry->second.first;
		if (!is_symbolic_temp(dependency.tmp_id)) {
			temps_to_process.emplace(std::make_pair(dependency, vex_setter_instr));
		}
	}
	while (!temps_to_process.empty()) {
		auto &vex_tmp = temps_to_process.front();
		if (vex_tmp.second != instr_addr) {
			auto &tmp_instr_slice = instr_slice_details_map.at(vex_tmp.second);
			auto &tmp_instr_details = curr_block_taint_entry.block_instrs_taint_data_map.at(vex_tmp.second);
			instr_slice_details.concrete_registers.insert(tmp_instr_slice.concrete_registers.begin(), tmp_instr_slice.concrete_registers.end());
			instr_slice_details.dependent_instrs.insert(tmp_instr_slice.dependent_instrs.begin(), tmp_instr_slice.dependent_instrs.end());
			instr_slice_details.dependent_instrs.emplace(compute_instr_details(vex_tmp.second, tmp_instr_details));
		}
		temps_to_process.pop();
		for (auto &dep_vex_tmp: curr_block_taint_entry.vex_temp_deps.at(vex_tmp.first).second) {
			auto vex_temp_deps_entry = curr_block_taint_entry.vex_temp_deps.find(dep_vex_tmp);
			if (vex_temp_deps_entry == curr_block_taint_entry.vex_temp_deps.end()) {
				// No dependency entries for this VEX temp
				continue;
			}
			address_t vex_setter_instr = vex_temp_deps_entry->second.first;
			temps_to_process.push(std::make_pair(dep_vex_tmp, vex_setter_instr));
		}
	}
	instr_slice_details_map.emplace(instr_addr, instr_slice_details);
	return;
}

block_taint_entry_t State::process_vex_block(IRSB *vex_block, address_t address) {
	block_taint_entry_t block_taint_entry;
	instruction_taint_entry_t instruction_taint_entry;
	bool started_processing_instructions;
	address_t curr_instr_addr;

	started_processing_instructions = false;
	block_taint_entry.has_unsupported_stmt_or_expr_type = false;
	for (auto i = 0; i < vex_block->stmts_used; i++) {
		auto stmt = vex_block->stmts[i];
		switch (stmt->tag) {
			case Ist_Put:
			{
				taint_entity_t sink;
				std::unordered_set<taint_entity_t> srcs;
				std::pair<vex_reg_offset_t, std::pair<int64_t, bool>> modified_reg_data;

				sink.entity_type = TAINT_ENTITY_REG;
				sink.instr_addr = curr_instr_addr;
				sink.reg_offset = stmt->Ist.Put.offset;
				modified_reg_data.first = stmt->Ist.Put.offset;
				modified_reg_data.second.second = false;
				auto result = process_vex_expr(stmt->Ist.Put.data, vex_block->tyenv, curr_instr_addr, false);
				if (result.has_unsupported_expr) {
					block_taint_entry.has_unsupported_stmt_or_expr_type = true;
					block_taint_entry.unsupported_stmt_stop_reason = result.unsupported_expr_stop_reason;
					break;
				}
				sink.value_size = result.value_size;
				modified_reg_data.second.first = sink.value_size;
				// Flatten list of taint sources and also save them as dependencies of instruction
				// TODO: Should we not save dependencies if sink is an artificial register?
				for (auto &entry: result.taint_sources) {
					srcs.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				instruction_taint_entry.taint_sink_src_map.emplace_back(sink, srcs);
				instruction_taint_entry.mem_read_size += result.mem_read_size;
				instruction_taint_entry.has_memory_read |= (result.mem_read_size != 0);
				// Check if sink is also source of taint
				if (result.taint_sources.at(sink.entity_type).count(sink)) {
					modified_reg_data.second.second = true;
				}
				// Store ITE condition entities. Also, store them as dependencies of instruction.
				for (auto &entry: result.ite_cond_entities) {
					instruction_taint_entry.ite_cond_entity_list.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				if ((modified_reg_data.first != arch_pc_reg_vex_offset()) && reg_instr_slice.count(modified_reg_data.first) != 0) {
					instruction_taint_entry.modified_regs.emplace_back(modified_reg_data);
				}
				break;
			}
			case Ist_WrTmp:
			{
				taint_entity_t sink;
				std::unordered_set<taint_entity_t> srcs;

				sink.entity_type = TAINT_ENTITY_TMP;
				sink.instr_addr = curr_instr_addr;
				sink.tmp_id = stmt->Ist.WrTmp.tmp;
				auto sink_type = vex_block->tyenv->types[sink.tmp_id];
				if (sink_type == Ity_I1) {
					sink.value_size = 0;
				}
				else {
					sink.value_size = sizeofIRType(sink_type);
				}
				auto result = process_vex_expr(stmt->Ist.WrTmp.data, vex_block->tyenv, curr_instr_addr, false);
				if (result.has_unsupported_expr) {
					block_taint_entry.has_unsupported_stmt_or_expr_type = true;
					block_taint_entry.unsupported_stmt_stop_reason = result.unsupported_expr_stop_reason;
					break;
				}
				// Store VEX temp dependencies details
				auto vex_temp_dep_data = std::make_pair(curr_instr_addr, result.taint_sources.at(TAINT_ENTITY_TMP));
				auto &vex_temp_ite_deps = result.ite_cond_entities.at(TAINT_ENTITY_TMP);
				vex_temp_dep_data.second.insert(vex_temp_ite_deps.begin(), vex_temp_ite_deps.end());
				for (auto &mem_taint_source: result.taint_sources.at(TAINT_ENTITY_MEM)) {
					for (auto &mem_ref_entity: mem_taint_source.mem_ref_entity_list) {
						instruction_taint_entry.dependencies.at(mem_ref_entity.entity_type).emplace(mem_ref_entity);
						if (mem_ref_entity.entity_type ==TAINT_ENTITY_TMP) {
							vex_temp_dep_data.second.emplace(mem_ref_entity);
						}
					}
				}
				block_taint_entry.vex_temp_deps.emplace(sink, vex_temp_dep_data);
				// Flatten list of taint sources and also save them as dependencies of instruction
				for (auto &entry: result.taint_sources) {
					srcs.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				instruction_taint_entry.taint_sink_src_map.emplace_back(sink, srcs);
				instruction_taint_entry.mem_read_size += result.mem_read_size;
				instruction_taint_entry.has_memory_read |= (result.mem_read_size != 0);
				// Store ITE condition entities. Also, store them as dependencies of instruction.
				for (auto &entry: result.ite_cond_entities) {
					instruction_taint_entry.ite_cond_entity_list.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				break;
			}
			case Ist_Store:
			{
				taint_entity_t sink;
				std::unordered_set<taint_entity_t> srcs;

				sink.entity_type = TAINT_ENTITY_MEM;
				sink.instr_addr = curr_instr_addr;
				instruction_taint_entry.has_memory_write = true;
				auto result = process_vex_expr(stmt->Ist.Store.addr, vex_block->tyenv, curr_instr_addr, false);
				if (result.has_unsupported_expr) {
					block_taint_entry.has_unsupported_stmt_or_expr_type = true;
					block_taint_entry.unsupported_stmt_stop_reason = result.unsupported_expr_stop_reason;
					break;
				}
				// TODO: What if memory addresses have ITE expressions in them?
				for (auto &entry: result.taint_sources) {
					sink.mem_ref_entity_list.insert(sink.mem_ref_entity_list.end(), entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				instruction_taint_entry.mem_read_size += result.mem_read_size;
				instruction_taint_entry.has_memory_read |= (result.mem_read_size != 0);

				result = process_vex_expr(stmt->Ist.Store.data, vex_block->tyenv, curr_instr_addr, false);
				if (result.has_unsupported_expr) {
					block_taint_entry.has_unsupported_stmt_or_expr_type = true;
					block_taint_entry.unsupported_stmt_stop_reason = result.unsupported_expr_stop_reason;
					break;
				}
				sink.value_size = result.value_size;
				// Flatten list of taint sources and also save them as dependencies of instruction
				for (auto &entry: result.taint_sources) {
					srcs.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				instruction_taint_entry.taint_sink_src_map.emplace_back(sink, srcs);
				instruction_taint_entry.mem_read_size += result.mem_read_size;
				instruction_taint_entry.has_memory_read |= (result.mem_read_size != 0);

				// Store ITE condition entities. Also, store them as dependencies of instruction.
				for (auto &entry: result.ite_cond_entities) {
					instruction_taint_entry.ite_cond_entity_list.insert(entry.second.begin(), entry.second.end());
					instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				break;
			}
			case Ist_Exit:
			{
				auto result = process_vex_expr(stmt->Ist.Exit.guard, vex_block->tyenv, curr_instr_addr, true);
				if (result.has_unsupported_expr) {
					block_taint_entry.has_unsupported_stmt_or_expr_type = true;
					block_taint_entry.unsupported_stmt_stop_reason = result.unsupported_expr_stop_reason;
					break;
				}
				for (auto &entry: result.taint_sources) {
					block_taint_entry.exit_stmt_guard_expr_deps.insert(entry.second.begin(), entry.second.end());
				}
				block_taint_entry.exit_stmt_instr_addr = curr_instr_addr;
				instruction_taint_entry.mem_read_size += result.mem_read_size;
				instruction_taint_entry.has_memory_read |= (result.mem_read_size != 0);
				break;
			}
			case Ist_IMark:
			{
				// Save dependencies of previous instruction and clear it
				if (started_processing_instructions) {
					// TODO: Many instructions will not have dependencies. Can we save memory by not storing info for them?
					block_taint_entry.block_instrs_taint_data_map.emplace(curr_instr_addr, instruction_taint_entry);
				}
				instruction_taint_entry.reset();
				curr_instr_addr = stmt->Ist.IMark.addr;
				started_processing_instructions = true;
				break;
			}
			case Ist_PutI:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_PUTI;
				break;
			}
			case Ist_StoreG:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_STOREG;
				break;
			}
			case Ist_LoadG:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_LOADG;
				break;
			}
			case Ist_CAS:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_CAS;
				break;
			}
			case Ist_LLSC:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_LLSC;
				break;
			}
			case Ist_Dirty:
			{
				// TODO
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_DIRTY;
				break;
			}
			case Ist_MBE:
			case Ist_NoOp:
			case Ist_AbiHint:
				break;
			default:
			{
				fprintf(stderr, "[sim_unicorn] Unsupported statement type encountered: ");
				fprintf(stderr, "Block: 0x%zx, statement index: %d, statement type: %u\n", address, i, stmt->tag);
				block_taint_entry.has_unsupported_stmt_or_expr_type = true;
				block_taint_entry.unsupported_stmt_stop_reason = STOP_UNSUPPORTED_STMT_UNKNOWN;
				break;
			}
		}
	}
	// Process block default exit target
	auto block_next_taint_sources = process_vex_expr(vex_block->next, vex_block->tyenv, curr_instr_addr, false);
	if (block_next_taint_sources.has_unsupported_expr) {
		block_taint_entry.has_unsupported_stmt_or_expr_type = true;
		block_taint_entry.unsupported_stmt_stop_reason = block_next_taint_sources.unsupported_expr_stop_reason;
	}
	else {
		for (auto &entry: block_next_taint_sources.taint_sources) {
			block_taint_entry.block_next_entities.insert(entry.second.begin(), entry.second.end());
			instruction_taint_entry.dependencies.at(entry.first).insert(entry.second.begin(), entry.second.end());
		}
		instruction_taint_entry.mem_read_size += block_next_taint_sources.mem_read_size;
		instruction_taint_entry.has_memory_read |= (block_next_taint_sources.mem_read_size != 0);
	}
	// Save last instruction's entry
	block_taint_entry.block_instrs_taint_data_map.emplace(curr_instr_addr, instruction_taint_entry);
	return block_taint_entry;
}

std::set<instr_details_t> State::get_list_of_dep_instrs(const instr_details_t &instr) const {
	std::set<instr_details_t> instrs;
	for (auto &dep_instr: instr.instr_deps) {
		auto result = get_list_of_dep_instrs(dep_instr);
		instrs.insert(result.begin(), result.end());
		instrs.insert(dep_instr);
	}
	return instrs;
}

void State::get_register_value(uint64_t vex_reg_offset, uint8_t *out_reg_value) const {
	uint64_t reg_value;
	if (cpu_flags_register != -1) {
		// Check if VEX register is actually a CPU flag
		auto cpu_flags_entry = cpu_flags.find(vex_reg_offset);
		if (cpu_flags_entry != cpu_flags.end()) {
			uc_reg_read(uc, cpu_flags_register, &reg_value);
			if ((reg_value & cpu_flags_entry->second) == 1) {
				// This hack assumes that a flag register is not MAX_REGISTER_BYTE_SIZE bytes long
				// so that it works on both big and little endian registers.
				out_reg_value[0] = 1;
				out_reg_value[MAX_REGISTER_BYTE_SIZE - 1] = 1;
			}
			return;
		}
	}
	uc_reg_read(uc, vex_to_unicorn_map.at(vex_reg_offset), out_reg_value);
	return;
}

// Returns a pair (taint sources, list of taint entities in ITE condition expression)
processed_vex_expr_t State::process_vex_expr(IRExpr *expr, IRTypeEnv *vex_block_tyenv, address_t instr_addr, bool is_exit_stmt) {
	processed_vex_expr_t result;
	result.reset();
	switch (expr->tag) {
		case Iex_RdTmp:
		{
			taint_entity_t taint_entity;
			taint_entity.entity_type = TAINT_ENTITY_TMP;
			taint_entity.tmp_id = expr->Iex.RdTmp.tmp;
			taint_entity.instr_addr = instr_addr;
			taint_entity.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			result.taint_sources.at(TAINT_ENTITY_TMP).emplace(taint_entity);
			result.value_size = taint_entity.value_size;
			break;
		}
		case Iex_Get:
		{
			taint_entity_t taint_entity;
			taint_entity.entity_type = TAINT_ENTITY_REG;
			taint_entity.reg_offset = expr->Iex.Get.offset;
			taint_entity.instr_addr = instr_addr;
			taint_entity.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			result.taint_sources.at(TAINT_ENTITY_REG).emplace(taint_entity);
			result.value_size = taint_entity.value_size;
			break;
		}
		case Iex_Unop:
		{
			auto temp = process_vex_expr(expr->Iex.Unop.arg, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);;
			break;
		}
		case Iex_Binop:
		{
			auto temp = process_vex_expr(expr->Iex.Binop.arg1, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Binop.arg2, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);;
			break;
		}
		case Iex_Triop:
		{
			auto temp = process_vex_expr(expr->Iex.Triop.details->arg1, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Triop.details->arg2, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Triop.details->arg3, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_Qop:
		{
			auto temp = process_vex_expr(expr->Iex.Qop.details->arg1, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Qop.details->arg2, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Qop.details->arg3, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.Qop.details->arg4, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_ITE:
		{
			// We store the taint entities in the condition for ITE separately in order to check
			// if condition is symbolic and stop concrete execution if it is. However for VEX
			// exit statement, we don't need to store it separately since we process only the
			// guard condition for Exit statements
			auto temp = process_vex_expr(expr->Iex.ITE.cond, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			if (is_exit_stmt) {
				for (auto &entry: temp.taint_sources) {
					result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				for (auto &entry: temp.ite_cond_entities) {
					result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
			}
			else {
				for (auto &entry: temp.taint_sources) {
					result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				for (auto &entry: temp.ite_cond_entities) {
					result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.ITE.iffalse, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;

			temp = process_vex_expr(expr->Iex.ITE.iftrue, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			for (auto &entry: temp.taint_sources) {
				result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			for (auto &entry: temp.ite_cond_entities) {
				result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
			}
			result.mem_read_size += temp.mem_read_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_CCall:
		{
			IRExpr **ccall_args = expr->Iex.CCall.args;
			for (auto i = 0; ccall_args[i]; i++) {
				auto temp = process_vex_expr(ccall_args[i], vex_block_tyenv, instr_addr, false);
				if (temp.has_unsupported_expr) {
					result.has_unsupported_expr = true;
					result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
					break;
				}
				for (auto &entry: temp.taint_sources) {
					result.taint_sources.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				for (auto &entry: temp.ite_cond_entities) {
					result.ite_cond_entities.at(entry.first).insert(entry.second.begin(), entry.second.end());
				}
				result.mem_read_size += temp.mem_read_size;
			}
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_Load:
		{
			auto temp = process_vex_expr(expr->Iex.Load.addr, vex_block_tyenv, instr_addr, false);
			if (temp.has_unsupported_expr) {
				result.has_unsupported_expr = true;
				result.unsupported_expr_stop_reason = temp.unsupported_expr_stop_reason;
				break;
			}
			// TODO: What if memory addresses have ITE expressions in them?
			taint_entity_t source;
			source.entity_type = TAINT_ENTITY_MEM;
			for (auto &entry: temp.taint_sources) {
				source.mem_ref_entity_list.insert(source.mem_ref_entity_list.end(), entry.second.begin(), entry.second.end());
			}
			source.instr_addr = instr_addr;
			result.taint_sources.at(TAINT_ENTITY_MEM).emplace(source);
			// Calculate number of bytes read. unicorn sometimes triggers read hook multiple times for the same read
			result.mem_read_size += temp.mem_read_size;
			// TODO: Will there be a 1 bit read from memory?
			auto load_size = sizeofIRType(expr->Iex.Load.ty);
			result.mem_read_size += load_size;
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_GetI:
		{
			// TODO
			result.has_unsupported_expr = true;
			result.unsupported_expr_stop_reason = STOP_UNSUPPORTED_EXPR_GETI;
			break;
		}
		case Iex_Const:
		{
			result.value_size = get_vex_expr_result_size(expr, vex_block_tyenv);
			break;
		}
		case Iex_VECRET:
		case Iex_GSPTR:
		case Iex_Binder:
			break;
		default:
		{
			fprintf(stderr, "[sim_unicorn] Unsupported expression type encountered: %u\n", expr->tag);
			result.has_unsupported_expr = true;
			result.unsupported_expr_stop_reason = STOP_UNSUPPORTED_EXPR_UNKNOWN;
			break;
		}
	}
	return result;
}

// Determine cumulative result of taint statuses of a set of taint entities
// EG: This is useful to determine the taint status of a taint sink given it's taint sources
taint_status_result_t State::get_final_taint_status(const std::unordered_set<taint_entity_t> &taint_sources) const {
	bool is_symbolic = false;
	for (auto &taint_source: taint_sources) {
		if (taint_source.entity_type == TAINT_ENTITY_NONE) {
			continue;
		}
		else if ((taint_source.entity_type == TAINT_ENTITY_REG) &&
		  (is_symbolic_register(taint_source.reg_offset, taint_source.value_size))) {
			  // Register is symbolic. Continue checking for read from symbolic address
			  is_symbolic = true;
		}
		else if ((taint_source.entity_type == TAINT_ENTITY_TMP) && (is_symbolic_temp(taint_source.tmp_id))) {
			// Temp is symbolic. Continue checking for read from a symbolic address
			is_symbolic = true;
		}
		else if (taint_source.entity_type == TAINT_ENTITY_MEM) {
			// Check if the memory address being read from is symbolic
			auto mem_address_status = get_final_taint_status(taint_source.mem_ref_entity_list);
			if (mem_address_status == TAINT_STATUS_SYMBOLIC) {
				// Address is symbolic. We have to stop concrete execution and so can stop analysing
				return TAINT_STATUS_DEPENDS_ON_READ_FROM_SYMBOLIC_ADDR;
			}
			else {
				// Address is concrete so we check result of the memory read
				mem_read_result_t mem_read_result;
				try {
					mem_read_result = mem_reads_map.at(taint_source.instr_addr);
				}
				catch (std::out_of_range const&) {
					assert(false && "[sim_unicorn] Taint sink depends on a read not executed yet! This should not happen!");
				}
				is_symbolic = mem_read_result.is_mem_read_symbolic;
			}
		}
	}
	if (is_symbolic) {
		return TAINT_STATUS_SYMBOLIC;
	}
	return TAINT_STATUS_CONCRETE;
}

// A vector version of get_final_taint_status for checking mem_ref_entity_list which can't be an
// unordered_set
taint_status_result_t State::get_final_taint_status(const std::vector<taint_entity_t> &taint_sources) const {
	std::unordered_set<taint_entity_t> taint_sources_set(taint_sources.begin(), taint_sources.end());
	return get_final_taint_status(taint_sources_set);
}

int32_t State::get_vex_expr_result_size(IRExpr *expr, IRTypeEnv* tyenv) const {
	auto expr_type = typeOfIRExpr(tyenv, expr);
	if (expr_type == Ity_I1) {
		return 0;
	}
	return sizeofIRType(expr_type);
}

void State::mark_register_symbolic(vex_reg_offset_t reg_offset, int64_t reg_size) {
	// Mark register as symbolic in the state in current block
	if (is_blacklisted_register(reg_offset)) {
		return;
	}
	else if (cpu_flags.find(reg_offset) != cpu_flags.end()) {
		block_symbolic_registers.emplace(reg_offset);
		block_concrete_registers.erase(reg_offset);
	}
	else {
		for (auto i = 0; i < reg_size; i++) {
			block_symbolic_registers.emplace(reg_offset + i);
			block_concrete_registers.erase(reg_offset + i);
		}
	}
	return;
}

void State::mark_temp_symbolic(vex_tmp_id_t temp_id) {
	// Mark VEX temp as symbolic in current block
	block_symbolic_temps.emplace(temp_id);
	return;
}

void State::mark_register_concrete(vex_reg_offset_t reg_offset, int64_t reg_size) {
	// Mark register as concrete in the current block
	if (is_blacklisted_register(reg_offset)) {
		return;
	}
	else if (cpu_flags.find(reg_offset) != cpu_flags.end()) {
		block_symbolic_registers.erase(reg_offset);
		block_concrete_registers.emplace(reg_offset);
	}
	else {
		for (auto i = 0; i < reg_size; i++) {
			block_symbolic_registers.erase(reg_offset + i);
			block_concrete_registers.emplace(reg_offset + i);
		}
	}
	return;
}

bool State::is_symbolic_register(vex_reg_offset_t reg_offset, int64_t reg_size) const {
	// We check if this register is symbolic or concrete in the block level taint statuses since
	// those are more recent. If not found in either, check the state's symbolic register list.
	// TODO: Is checking only first byte of artificial and blacklisted registers to determine if they are symbolic fine
	// or should all be checked?
	if ((cpu_flags.find(reg_offset) != cpu_flags.end()) || (artificial_vex_registers.count(reg_offset) > 0)
	    || (blacklisted_registers.count(reg_offset) > 0)) {
		if (block_symbolic_registers.count(reg_offset) > 0) {
			return true;
		}
		else if (block_concrete_registers.count(reg_offset) > 0) {
			return false;
		}
		else if (symbolic_registers.count(reg_offset) > 0) {
			return true;
		}
		return false;
	}
	// The register is not a CPU flag and so we check every byte of the register
	for (auto i = 0; i < reg_size; i++) {
		// If any of the register's bytes are symbolic, we deem the register to be symbolic
		if (block_symbolic_registers.count(reg_offset + i) > 0) {
			return true;
		}
	}
	bool is_concrete = true;
	for (auto i = 0; i < reg_size; i++) {
		if (block_concrete_registers.count(reg_offset) == 0) {
			is_concrete = false;
			break;
		}
	}
	if (is_concrete) {
		// All bytes of register are concrete and so the register is concrete
		return false;
	}
	// If we reach here, it means that the register is not marked symbolic or concrete in the block
	// level taint status tracker. We check the state's symbolic register list.
	for (auto i = 0; i < reg_size; i++) {
		if (symbolic_registers.count(reg_offset + i) > 0) {
			return true;
		}
	}
	return false;
}

bool State::is_symbolic_temp(vex_tmp_id_t temp_id) const {
	return (block_symbolic_temps.count(temp_id) > 0);
}

void State::propagate_taints() {
	if (is_symbolic_tracking_disabled()) {
		// We're not checking symbolic registers so no need to propagate taints
		return;
	}
	auto& block_taint_entry = this->block_taint_cache.at(curr_block_details.block_addr);
	if (((symbolic_registers.size() > 0) || (block_symbolic_registers.size() > 0))
		&& block_taint_entry.has_unsupported_stmt_or_expr_type) {
		// There are symbolic registers and VEX statements in block for which taint propagation
		// is not supported. Stop concrete execution.
		stop(block_taint_entry.unsupported_stmt_stop_reason);
		return;
	}
	// Resume propagating taints using symbolic_registers and symbolic_temps from where we paused
	auto instr_taint_data_entries_it = block_taint_entry.block_instrs_taint_data_map.find(taint_engine_next_instr_address);
	auto instr_taint_data_stop_it = block_taint_entry.block_instrs_taint_data_map.end();
	// We continue propagating taint until we encounter 1) a memory read, 2) end of block or
	// 3) a stop state for concrete execution
	for (; instr_taint_data_entries_it != instr_taint_data_stop_it && !stopped; ++instr_taint_data_entries_it) {
		address_t curr_instr_addr = instr_taint_data_entries_it->first;
		auto& curr_instr_taint_entry = instr_taint_data_entries_it->second;
		if (curr_instr_taint_entry.has_memory_read) {
			// Pause taint propagation to process the memory read and continue from instruction
			// after the memory read.
			taint_engine_stop_mem_read_instruction = curr_instr_addr;
			taint_engine_stop_mem_read_size = instr_taint_data_entries_it->second.mem_read_size;
			taint_engine_next_instr_address = std::next(instr_taint_data_entries_it)->first;
			return;
		}
		if ((symbolic_registers.size() == 0) && (block_symbolic_registers.size() == 0) && (block_symbolic_temps.size() == 0)) {
			// There are no symbolic registers so no taint to propagate. Mark any memory writes
			// as concrete and update slice of registers.
			if (curr_instr_taint_entry.has_memory_write) {
				mem_writes_taint_map.emplace(curr_instr_addr, false);
			}
			compute_slice_of_instrs(curr_instr_addr, curr_instr_taint_entry);
			update_register_slice(curr_instr_addr, curr_instr_taint_entry);
			continue;
		}
		compute_slice_of_instrs(curr_instr_addr, curr_instr_taint_entry);
		propagate_taint_of_one_instr(curr_instr_addr, curr_instr_taint_entry);
		update_register_slice(curr_instr_addr, curr_instr_taint_entry);
	}
	// If we reached here, execution has reached the end of the block
	if (!stopped) {
		if (curr_block_details.vex_lift_failed && ((symbolic_registers.size() > 0) || (block_symbolic_registers.size() > 0))) {
			// There are symbolic registers but VEX lift failed so we can't determine
			// status of guard condition
			stop(STOP_VEX_LIFT_FAILED);
			return;
		}
		else if (is_block_exit_guard_symbolic()) {
			stop(STOP_SYMBOLIC_BLOCK_EXIT_CONDITION);
		}
		else if (is_block_next_target_symbolic()) {
			stop(STOP_SYMBOLIC_BLOCK_EXIT_TARGET);
		}
	}
	return;
}

void State::propagate_taint_of_mem_read_instr_and_continue(const address_t instr_addr) {
	if (is_symbolic_tracking_disabled()) {
		// We're not checking symbolic registers so no need to propagate taints
		return;
	}
	auto mem_read_result = mem_reads_map.at(instr_addr);
	if (curr_block_details.vex_lift_failed) {
		if (mem_read_result.is_mem_read_symbolic || (symbolic_registers.size() > 0) || (block_symbolic_registers.size() > 0)) {
			// Either the memory value is symbolic or there are symbolic registers: thus, taint
			// status of registers could change. But since VEX lift failed, the taint relations
			// are not known and so we can't propagate taint. Stop concrete execution.
			stop(STOP_VEX_LIFT_FAILED);
			return;
		}
		else {
			// We cannot propagate taint since VEX lift failed and so we stop here. But, since
			// there are no symbolic values, we do need need to propagate taint. Of course,
			// the register slices cannot be updated but those won't be needed since this
			// block will never be executed partially concretely and rest in VEX engine.
			return;
		}
	}
	if (mem_read_result.read_size != taint_engine_stop_mem_read_size) {
		// There are more bytes to be read by this instruction. We do not propagate taint until bytes are read
		// Sometimes reads are split across multiple reads hooks in unicorn.
		return;
	}

	// There are no more pending reads at this instruction. Now we can propagate taint.
	// This allows us to also handle cases when only some of the memory reads are symbolic: we treat all as symbolic
	// and overtaint.
	auto& block_taint_entry = block_taint_cache.at(curr_block_details.block_addr);
	auto& instr_taint_data_entry = block_taint_entry.block_instrs_taint_data_map.at(instr_addr);
	if (mem_read_result.is_mem_read_symbolic || (symbolic_registers.size() > 0) || (block_symbolic_registers.size() > 0) ||
	  block_symbolic_temps.size() > 0) {
		if (block_taint_entry.has_unsupported_stmt_or_expr_type) {
			// There are symbolic registers and/or memory read was symbolic and there are VEX
			// statements in block for which taint propagation is not supported.
			stop(block_taint_entry.unsupported_stmt_stop_reason);
			return;
		}
		compute_slice_of_instrs(instr_addr, instr_taint_data_entry);
		propagate_taint_of_one_instr(instr_addr, instr_taint_data_entry);
	}
	if (instr_slice_details_map.count(instr_addr) == 0) {
		compute_slice_of_instrs(instr_addr, instr_taint_data_entry);
	}
	update_register_slice(instr_addr, instr_taint_data_entry);
	if (!stopped) {
		continue_propagating_taint();
	}
	return;
}

void State::propagate_taint_of_one_instr(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry) {
	instr_details_t instr_details;
	bool is_instr_symbolic;

	is_instr_symbolic = false;
	instr_details = compute_instr_details(instr_addr, instr_taint_entry);
	if (instr_details.has_symbolic_memory_dep) {
		is_instr_symbolic = true;
	}
	for (auto &taint_data_entry: instr_taint_entry.taint_sink_src_map) {
		taint_entity_t taint_sink = taint_data_entry.first;
		std::unordered_set<taint_entity_t> taint_srcs = taint_data_entry.second;
		if (taint_sink.entity_type == TAINT_ENTITY_MEM) {
			auto addr_taint_status = get_final_taint_status(taint_sink.mem_ref_entity_list);
			// Check if address written to is symbolic or is read from memory
			if (addr_taint_status != TAINT_STATUS_CONCRETE) {
				stop(STOP_SYMBOLIC_WRITE_ADDR);
				return;
			}
			auto sink_taint_status = get_final_taint_status(taint_srcs);
			if (sink_taint_status == TAINT_STATUS_DEPENDS_ON_READ_FROM_SYMBOLIC_ADDR) {
				stop(STOP_SYMBOLIC_READ_ADDR);
				return;
			}
			if (sink_taint_status == TAINT_STATUS_SYMBOLIC) {
				// Save the memory location written to be marked as symbolic in write hook
				// If memory write already exists, we overtaint and mark all writes as symbolic
				mem_writes_taint_map[taint_sink.instr_addr] = true;
				// Mark instruction as needing symbolic execution
				is_instr_symbolic = true;
			}
			else {
				// Save the memory location(s) written to be marked as concrete in the write
				// hook only if it is not a previously seen write
				mem_writes_taint_map.emplace(taint_sink.instr_addr, false);
			}
		}
		else if (taint_sink.entity_type != TAINT_ENTITY_NONE) {
			taint_status_result_t final_taint_status = get_final_taint_status(taint_srcs);
			if (final_taint_status == TAINT_STATUS_DEPENDS_ON_READ_FROM_SYMBOLIC_ADDR) {
				stop(STOP_SYMBOLIC_READ_ADDR);
				return;
			}
			else if (final_taint_status == TAINT_STATUS_SYMBOLIC) {
				if ((taint_sink.entity_type == TAINT_ENTITY_REG) && (taint_sink.reg_offset == arch_pc_reg_vex_offset())) {
					stop(STOP_SYMBOLIC_PC);
					return;
				}

				// Mark instruction as needing symbolic execution
				is_instr_symbolic = true;

				// Mark sink as symbolic
				if (taint_sink.entity_type == TAINT_ENTITY_REG) {
					mark_register_symbolic(taint_sink.reg_offset, taint_sink.value_size);
				}
				else {
					mark_temp_symbolic(taint_sink.tmp_id);
				}
			}
			else if ((taint_sink.entity_type == TAINT_ENTITY_REG) && (taint_sink.reg_offset != arch_pc_reg_vex_offset())) {
				// Mark register as concrete since none of it's dependencies are symbolic. Also update it's slice.
				mark_register_concrete(taint_sink.reg_offset, taint_sink.value_size);
			}
		}
		auto ite_cond_taint_status = get_final_taint_status(instr_taint_entry.ite_cond_entity_list);
		if (ite_cond_taint_status != TAINT_STATUS_CONCRETE) {
			stop(STOP_SYMBOLIC_CONDITION);
			return;
		}
	}
	if (is_instr_symbolic) {
		auto &instr_slice_details = instr_slice_details_map.at(instr_addr);
		for (auto &reg: instr_slice_details.concrete_registers) {
			auto reg_offset = reg.first;
			auto reg_size = reg.second;
			auto reg_val = block_start_reg_values.at(reg_offset);
			reg_val.size = reg_size;
			instr_details.reg_deps.insert(reg_val);
		}
		instr_details.instr_deps.insert(instr_details.instr_deps.end(), instr_slice_details.dependent_instrs.begin(), instr_slice_details.dependent_instrs.end());
		auto result = find_symbolic_mem_deps(instr_details);
		instr_details.symbolic_mem_deps.insert(instr_details.symbolic_mem_deps.end(), result.begin(), result.end());
		curr_block_details.symbolic_instrs.emplace_back(instr_details);
	}
	return;
}

instr_details_t State::compute_instr_details(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry) {
	instr_details_t instr_details;
	instr_details.instr_addr = instr_addr;
	if (instr_taint_entry.has_memory_read) {
		auto mem_read_result = mem_reads_map.at(instr_addr);
		if (!mem_read_result.is_mem_read_symbolic) {
			instr_details.has_concrete_memory_dep = true;
			instr_details.has_symbolic_memory_dep = false;
		}
		else {
			instr_details.has_concrete_memory_dep = false;
			instr_details.has_symbolic_memory_dep = true;
		}
	}
	else {
		instr_details.has_concrete_memory_dep = false;
		instr_details.has_symbolic_memory_dep = false;
	}
	return instr_details;
}

void State::read_memory_value(address_t address, uint64_t size, uint8_t *result, size_t result_size) const {
	memset(result, 0, result_size);
	uc_mem_read(uc, address, result, size);
	return;
}

void State::start_propagating_taint(address_t block_address, int32_t block_size) {
	curr_block_details.block_addr = block_address;
	curr_block_details.block_size = block_size;
	if (is_symbolic_tracking_disabled()) {
		// We're not checking symbolic registers so no need to propagate taints
		return;
	}
	if (this->block_taint_cache.find(block_address) == this->block_taint_cache.end()) {
		// Compute and cache taint sink-source relations for this block
		// Disable cross instruction optimization in IR so that dependencies of symbolic
		// instructions can be computed correctly.
		VexRegisterUpdates pxControl = VexRegUpdUnwindregsAtMemAccess;
		std::unique_ptr<uint8_t[]> instructions(new uint8_t[block_size]);
		address_t lift_address;

		if ((arch == UC_ARCH_ARM) && is_thumb_mode()) {
			lift_address = block_address | 1;
		}
		else {
			lift_address = block_address;
		}
		uc_mem_read(this->uc, lift_address, instructions.get(), block_size);
		VEXLiftResult *lift_ret = vex_lift(
			this->vex_guest, this->vex_archinfo, instructions.get(), lift_address, 99, block_size, 1, 0, 1,
			1, 0, pxControl
		);

		if ((lift_ret == NULL) || (lift_ret->size == 0)) {
			// Failed to lift block to VEX.
			if (symbolic_registers.size() > 0) {
				// There are symbolic registers but VEX lift failed so we can't propagate taint
				stop(STOP_VEX_LIFT_FAILED);
			}
			else {
				// There are no symbolic registers so attempt to execute block. Mark block as VEX lift failed.
				curr_block_details.vex_lift_failed = true;
			}
			return;
		}
		if ((arch == UC_ARCH_ARM) && (lift_ret->irsb->jumpkind == Ijk_Sys_syscall)) {
			// This block invokes a syscall. For now, such blocks are handled by VEX engine.
			stop(STOP_SYSCALL_ARM);
			return;
		}
		auto block_taint_entry = process_vex_block(lift_ret->irsb, block_address);
		// Add entry to taint relations cache
		block_taint_cache.emplace(block_address, block_taint_entry);
	}
	taint_engine_next_instr_address = block_address;
	block_symbolic_temps.clear();
	block_start_reg_values.clear();
	for (auto &reg_instr_slice_entry: reg_instr_slice) {
		// Clear slice for register
		reg_instr_slice_entry.second.clear();
		// Save value of all registers
		register_value_t reg_value;
		reg_value.offset = reg_instr_slice_entry.first;
		memset(reg_value.value, 0, MAX_REGISTER_BYTE_SIZE);
		get_register_value(reg_value.offset, reg_value.value);
		block_start_reg_values.emplace(reg_value.offset, reg_value);
		// Reset the slice of register
		reg_instr_slice_entry.second.clear();
	}
	for (auto &cpu_flag: cpu_flags) {
		register_value_t flag_value;
		flag_value.offset = cpu_flag.first;
		memset(flag_value.value, 0, MAX_REGISTER_BYTE_SIZE);
		get_register_value(cpu_flag.first, flag_value.value);
		block_start_reg_values.emplace(flag_value.offset, flag_value);
	}
	propagate_taints();
	return;
}

void State::continue_propagating_taint() {
	if (is_symbolic_tracking_disabled()) {
		// We're not checking symbolic registers so no need to propagate taints
		return;
	}
	if (curr_block_details.vex_lift_failed) {
		if ((symbolic_registers.size() > 0) || (block_symbolic_registers.size() > 0)) {
			// There are symbolic registers but VEX lift failed so we can't propagate taint
			stop(STOP_VEX_LIFT_FAILED);
			return;
		}
	}
	else {
		propagate_taints();
	}
	return;
}

std::vector<std::pair<address_t, uint64_t>> State::find_symbolic_mem_deps(instr_details_t &instr) const {
	std::vector<std::pair<address_t, uint64_t>> symbolic_mem_deps;
	for (auto &dep_instr: instr.instr_deps) {
		auto result = find_symbolic_mem_deps(dep_instr);
		symbolic_mem_deps.insert(symbolic_mem_deps.end(), result.begin(), result.end());
	}
	if (instr.has_symbolic_memory_dep) {
		for (auto &mem_value: mem_reads_map.at(instr.instr_addr).memory_values) {
			if (mem_value.is_value_symbolic) {
				symbolic_mem_deps.emplace_back(std::make_pair(mem_value.address, mem_value.size));
			}
		}
	}
	return symbolic_mem_deps;
}

void State::save_concrete_memory_deps(instr_details_t &instr) {
	if (instr.has_concrete_memory_dep) {
		archived_memory_values.emplace_back(mem_reads_map.at(instr.instr_addr).memory_values);
		instr.memory_values = &(archived_memory_values.back()[0]);
		instr.memory_values_count = archived_memory_values.back().size();
	}
	for (auto &dep_instr: instr.instr_deps) {
		save_concrete_memory_deps(dep_instr);
	}
	return;
}

void State::update_register_slice(address_t instr_addr, const instruction_taint_entry_t &curr_instr_taint_entry) {
	instr_details_t instr_details = compute_instr_details(instr_addr, curr_instr_taint_entry);
	for (auto &reg_entry: curr_instr_taint_entry.modified_regs) {
		vex_reg_offset_t reg_offset = reg_entry.first;
		auto reg_size = reg_entry.second.first;
		if ((reg_offset == arch_pc_reg_vex_offset()) || is_symbolic_register(reg_offset, reg_size)) {
			continue;
		}
		if (reg_instr_slice.find(reg_offset) == reg_instr_slice.end()) {
			continue;
		}
		// Check if register's new value depends on it's previous value
		if (!reg_entry.second.second) {
			reg_instr_slice.at(reg_offset).clear();
		}
		reg_instr_slice.at(reg_offset).emplace_back(instr_details);
	}
	return;
}

bool State::is_block_exit_guard_symbolic() const {
	auto& block_taint_entry = block_taint_cache.at(curr_block_details.block_addr);
	auto block_exit_guard_taint_status = get_final_taint_status(block_taint_entry.exit_stmt_guard_expr_deps);
	return (block_exit_guard_taint_status != TAINT_STATUS_CONCRETE);
}

bool State::is_block_next_target_symbolic() const {
	auto& block_taint_entry = block_taint_cache.at(curr_block_details.block_addr);
	auto block_next_target_taint_status = get_final_taint_status(block_taint_entry.block_next_entities);
	return (block_next_target_taint_status != TAINT_STATUS_CONCRETE);
}

bool State::check_symbolic_stack_mem_dependencies_liveness() const {
	// Stop concrete execution if a stack frame was deallocated and if any symbolic memory dependencies were present
	// on that stack frame.
	address_t curr_stack_top_addr = get_stack_pointer();
	if (curr_stack_top_addr <= prev_stack_top_addr) {
		// No change in stack frame and so no need to perform a liveness check.
		// TODO: What is stack growth direction is different?
		return true;
	}
	for (auto &block: blocks_with_symbolic_instrs) {
		for (auto &symbolic_instr: block.symbolic_instrs) {
			for (auto &symbolic_mem_dep: symbolic_instr.symbolic_mem_deps) {
				if ((curr_stack_top_addr > symbolic_mem_dep.first) && (symbolic_mem_dep.first > prev_stack_top_addr)) {
					// A symbolic memory value that this symbolic instruction depends on is no longer on the stack
					// and could be overwritten by future code. We stop concrete execution here to avoid that.
					return false;
				}
			}
		}
	}
	return true;
}

address_t State::get_instruction_pointer() const {
	address_t out = 0;
	int reg = arch_pc_reg();
	if (reg == -1) {
		out = 0;
	} else {
		uc_reg_read(uc, reg, &out);
	}

	return out;
}

address_t State::get_stack_pointer() const {
	address_t out = 0;
	int reg = arch_sp_reg();
	if (reg == -1) {
		out = 0;
	} else {
		uc_reg_read(uc, reg, &out);
	}

	return out;
}

static void hook_mem_read(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
	// uc_mem_read(uc, address, &value, size);
	// //LOG_D("mem_read [%#lx, %#lx] = %#lx", address, address + size);
	//LOG_D("mem_read [%#lx, %#lx]", address, address + size);
	State *state = (State *)user_data;
	memory_value_t memory_read_value;
	bool is_memory_value_symbolic;
	address_t curr_instr_addr;

	memory_read_value.reset();
	memory_read_value.address = address;
	memory_read_value.size = size;
	if (!state->is_symbolic_taint_propagation_disabled()) {
		// unicorn-engine doesn't update PC register in some cases and so it has incorrect value for current instruction
		// address. XMM reads sometimes trigger the hook multiple times and there could be multiple reads in one instruction
		// in some archs. So, we use the taint engine to determine instruction address correctly.
		// See https://github.com/unicorn-engine/unicorn/issues/1312, https://github.com/unicorn-engine/unicorn/pull/1257
		curr_instr_addr = state->get_taint_engine_stop_mem_read_instr_addr();
	}
	else {
		// Symbolic taint propagation is disabled so we get current instruction address from unicorn
		curr_instr_addr = state->get_instruction_pointer();
	}
	auto tainted = state->find_tainted(address, size);
	if (tainted != -1)
	{
		if (state->is_symbolic_tracking_disabled()) {
			// Symbolic register tracking is disabled but memory location has symbolic data.
			// We switch to VEX engine then.
			state->stop(STOP_SYMBOLIC_READ_SYMBOLIC_TRACKING_DISABLED);
			return;
		}
		is_memory_value_symbolic = true;
	}
	else
	{
		is_memory_value_symbolic = false;
		state->read_memory_value(address, size, memory_read_value.value, MAX_MEM_ACCESS_SIZE);
	}
	memory_read_value.is_value_symbolic = is_memory_value_symbolic;
	auto mem_reads_map_entry = state->mem_reads_map.find(curr_instr_addr);
	if (mem_reads_map_entry == state->mem_reads_map.end()) {
		mem_read_result_t mem_read_result;
		mem_read_result.memory_values.emplace_back(memory_read_value);
		mem_read_result.is_mem_read_symbolic = is_memory_value_symbolic;
		mem_read_result.read_size = size;
		state->mem_reads_map.emplace(curr_instr_addr, mem_read_result);
	}
	else {
		auto &mem_read_entry = state->mem_reads_map.at(curr_instr_addr);
		mem_read_entry.memory_values.emplace_back(memory_read_value);
		mem_read_entry.is_mem_read_symbolic |= is_memory_value_symbolic;
		mem_read_entry.read_size += size;
	}
	state->propagate_taint_of_mem_read_instr_and_continue(curr_instr_addr);
	return;
}

/*
 * the goal of hooking memory write is to determine the exact
 * positions of dirty bytes to writing chaneges back to angr
 * state. However if the hook is hit before mapping requested
 * page (as writable), we cannot find the bitmap for this page.
 * In this case, just mark all the position as clean (before
 * this access).
 */

static void hook_mem_write(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
	//LOG_D("mem_write [%#lx, %#lx]", address, address + size);
	State *state = (State *)user_data;

	if (state->ignore_next_selfmod) {
		// ...the self-modification gets repeated for internal qemu reasons
		state->ignore_next_selfmod = false;
	} else if ((address >= state->cur_address && address < state->cur_address + state->cur_size) ||
		// CODE IS SELF-MODIFYING: qemu will restart this basic block at this address.
		// discard the next block hook
		(state->cur_address >= address && state->cur_address < address + size)) {
		state->ignore_next_block = true;
	}

	state->handle_write(address, size, false);
}

static void hook_block(uc_engine *uc, uint64_t address, int32_t size, void *user_data) {
	//LOG_I("block [%#lx, %#lx]", address, address + size);

	State *state = (State *)user_data;
	if (state->ignore_next_block) {
		state->ignore_next_block = false;
		state->ignore_next_selfmod = true;
		return;
	}
	if (!state->check_symbolic_stack_mem_dependencies_liveness()) {
		state->stop(STOP_SYMBOLIC_MEM_DEP_NOT_LIVE, true);
		return;
	}
	state->commit();
	state->update_previous_stack_top();
	state->step(address, size);

	if (!state->stopped) {
		state->start_propagating_taint(address, size);
	}
	return;
}

static void hook_intr(uc_engine *uc, uint32_t intno, void *user_data) {
	State *state = (State *)user_data;
	state->interrupt_handled = false;

	if (state->arch == UC_ARCH_X86 && intno == 0x80) {
		// this is the ultimate hack for cgc -- it must be enabled by explitly setting the transmit sysno from python
		// basically an implementation of the cgc transmit syscall

		for (auto sr : state->symbolic_registers) {
			// eax,ecx,edx,ebx,esi
			if ((sr >= 8 && sr <= 23) || (sr >= 32 && sr <= 35)) return;
		}

		uint32_t sysno;
		uc_reg_read(uc, UC_X86_REG_EAX, &sysno);
		//printf("SYSCALL: %d\n", sysno);
		if (sysno == state->transmit_sysno) {
			//printf(".. TRANSMIT!\n");
			uint32_t fd, buf, count, tx_bytes;

			uc_reg_read(uc, UC_X86_REG_EBX, &fd);

			if (fd == 2) {
				// we won't try to handle fd 2 prints here, they are uncommon.
				return;
			} else if (fd == 0 || fd == 1) {
				uc_reg_read(uc, UC_X86_REG_ECX, &buf);
				uc_reg_read(uc, UC_X86_REG_EDX, &count);
				uc_reg_read(uc, UC_X86_REG_ESI, &tx_bytes);

				// ensure that the memory we're sending is not tainted
				// TODO: Can transmit also work with symbolic bytes?
				void *dup_buf = malloc(count);
				uint32_t tmp_tx;
				if (uc_mem_read(uc, buf, dup_buf, count) != UC_ERR_OK)
				{
					//printf("... fault on buf\n");
					free(dup_buf);
					return;
				}

				if (tx_bytes != 0 && uc_mem_read(uc, tx_bytes, &tmp_tx, 4) != UC_ERR_OK)
				{
					//printf("... fault on tx\n");
					free(dup_buf);
					return;
				}

				if (state->find_tainted(buf, count) != -1)
				{
					//printf("... symbolic data\n");
					free(dup_buf);
					return;
				}

				state->step(state->transmit_bbl_addr, 0, false);
				state->commit();
				if (state->stopped)
				{
					//printf("... stopped after step()\n");
					free(dup_buf);
					return;
				}

				uc_err err = uc_mem_write(uc, tx_bytes, &count, 4);
				if (tx_bytes != 0) state->handle_write(tx_bytes, 4, true);
				if (state->stopped) {
					return;
				}
				state->transmit_records.push_back({dup_buf, count});
				int result = 0;
				uc_reg_write(uc, UC_X86_REG_EAX, &result);
				state->symbolic_registers.erase(8);
				state->symbolic_registers.erase(9);
				state->symbolic_registers.erase(10);
				state->symbolic_registers.erase(11);
				state->interrupt_handled = true;
				state->syscall_count++;
				return;
			}
		}
	}
}

static bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
	State *state = (State *)user_data;
	uint64_t start = address & ~0xFFFULL;
	uint64_t end = (address + size - 1) & ~0xFFFULL;

	// only hook nonwritable pages
	if (type != UC_MEM_WRITE_UNMAPPED && state->map_cache(start, 0x1000) && (start == end || state->map_cache(end, 0x1000))) {
		//LOG_D("handle unmapped page natively");
		return true;
	}

	return false;
}

static bool hook_mem_prot(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data) {
	State *state = (State *)user_data;
	//printf("Segfault data: %d %#llx %d %#llx\n", type, address, size, value);
	state->stop(STOP_SEGFAULT);
	return true;
}

/*
 * C style bindings makes it simple and dirty
 */

extern "C"
State *simunicorn_alloc(uc_engine *uc, uint64_t cache_key) {
	State *state = new State(uc, cache_key);
	return state;
}

extern "C"
void simunicorn_dealloc(State *state) {
	delete state;
}

extern "C"
uint64_t *simunicorn_bbl_addrs(State *state) {
	return &(state->bbl_addrs[0]);
}

extern "C"
uint64_t *simunicorn_stack_pointers(State *state) {
	return &(state->stack_pointers[0]);
}

extern "C"
uint64_t simunicorn_bbl_addr_count(State *state) {
	return state->bbl_addrs.size();
}

extern "C"
uint64_t simunicorn_syscall_count(State *state) {
	return state->syscall_count;
}

extern "C"
void simunicorn_hook(State *state) {
	state->hook();
}

extern "C"
void simunicorn_unhook(State *state) {
	state->unhook();
}

extern "C"
uc_err simunicorn_start(State *state, uint64_t pc, uint64_t step) {
	return state->start(pc, step);
}

extern "C"
void simunicorn_stop(State *state, stop_t reason) {
	state->stop(reason);
}

extern "C"
mem_update_t *simunicorn_sync(State *state) {
	return state->sync();
}

extern "C"
uint64_t simunicorn_step(State *state) {
	return state->cur_steps;
}

extern "C"
void simunicorn_set_stops(State *state, uint64_t count, uint64_t *stops)
{
	state->set_stops(count, stops);
}

extern "C"
void simunicorn_activate_page(State *state, uint64_t address, uint8_t *taint, uint8_t *data) {
    state->page_activate(address, taint, data);
}

extern "C"
uint64_t simunicorn_executed_pages(State *state) { // this is HORRIBLE
	if (state->executed_pages_iterator == NULL) {
		state->executed_pages_iterator = new std::unordered_set<address_t>::iterator;
		*state->executed_pages_iterator = state->executed_pages.begin();
	}

	if (*state->executed_pages_iterator == state->executed_pages.end()) {
		delete state->executed_pages_iterator;
		state->executed_pages_iterator = NULL;
		return -1;
	}

	uint64_t out = **state->executed_pages_iterator;
	(*state->executed_pages_iterator)++;
	return out;
}

//
// Stop analysis
//

extern "C"
stop_details_t simunicorn_get_stop_details(State *state) {
	return state->stop_details;
}

//
// Symbolic register tracking
//

extern "C"
void simunicorn_symbolic_register_data(State *state, uint64_t count, uint64_t *offsets)
{
	state->symbolic_registers.clear();
	for (auto i = 0; i < count; i++) {
		state->symbolic_registers.insert(offsets[i]);
	}
}

extern "C"
uint64_t simunicorn_get_symbolic_registers(State *state, uint64_t *output)
{
	int i = 0;
	for (auto r : state->symbolic_registers) {
		output[i] = r;
		i++;
	}
	return i;
}

extern "C"
void simunicorn_enable_symbolic_reg_tracking(State *state, VexArch guest, VexArchInfo archinfo) {
	state->vex_guest = guest;
	state->vex_archinfo = archinfo;
}

extern "C"
void simunicorn_disable_symbolic_reg_tracking(State *state) {
	state->vex_guest = VexArch_INVALID;
}

//
// Concrete transmits
//

extern "C"
bool simunicorn_is_interrupt_handled(State *state) {
	return state->interrupt_handled;
}

extern "C"
void simunicorn_set_transmit_sysno(State *state, uint32_t sysno, uint64_t bbl_addr) {
	state->transmit_sysno = sysno;
	state->transmit_bbl_addr = bbl_addr;
}

extern "C"
transmit_record_t *simunicorn_process_transmit(State *state, uint32_t num) {
	if (num >= state->transmit_records.size()) {
		for (auto record_iter = state->transmit_records.begin();
				record_iter != state->transmit_records.end();
				record_iter++) {
			free(record_iter->data);
		}
		state->transmit_records.clear();
		return NULL;
	} else {
		transmit_record_t *out = &state->transmit_records[num];
		return out;
	}
}


/*
 * Page cache
 */

extern "C"
bool simunicorn_cache_page(State *state, uint64_t address, uint64_t length, char *bytes, uint64_t permissions) {
	//LOG_I("caching [%#lx, %#lx]", address, address + length);

	auto actual = state->cache_page(address, length, bytes, permissions);
	if (!state->map_cache(actual.first, actual.second)) {
		return false;
	}
	return true;
}

extern "C"
void simunicorn_uncache_pages_touching_region(State *state, uint64_t address, uint64_t length) {
	state->uncache_pages_touching_region(address, length);
}

extern "C"
void simunicorn_clear_page_cache(State *state) {
	state->clear_page_cache();
}

// Tracking settings
extern "C"
void simunicorn_set_tracking(State *state, bool track_bbls, bool track_stack) {
	state->track_bbls = track_bbls;
	state->track_stack = track_stack;
}

extern "C"
bool simunicorn_in_cache(State *state, uint64_t address) {
	return state->in_cache(address);
}

extern "C"
void simunicorn_set_map_callback(State *state, uc_cb_eventmem_t cb) {
    state->py_mem_callback = cb;
}

// VEX artificial registers list
extern "C"
void simunicorn_set_artificial_registers(State *state, uint64_t *offsets, uint64_t count) {
	state->artificial_vex_registers.clear();
	for (auto i = 0; i < count; i++) {
		state->artificial_vex_registers.emplace(offsets[i]);
	}
	return;
}

// VEX register offsets to unicorn register ID mappings
extern "C"
void simunicorn_set_vex_to_unicorn_reg_mappings(State *state, uint64_t *vex_offsets, uint64_t *unicorn_ids, uint64_t count) {
	state->vex_to_unicorn_map.clear();
	for (auto i = 0; i < count; i++) {
		state->vex_to_unicorn_map.emplace(vex_offsets[i], unicorn_ids[i]);
	}
	return;
}

// Mapping details for flags registers
extern "C"
void simunicorn_set_cpu_flags_details(State *state, uint64_t *flag_vex_id, uint64_t *bitmasks, uint64_t count) {
	state->cpu_flags.clear();
	for (auto i = 0; i < count; i++) {
		state->cpu_flags.emplace(flag_vex_id[i], bitmasks[i]);
	}
	return;
}

// Flag register ID in unicorn
extern "C"
void simunicorn_set_unicorn_flags_register_id(State *state, int64_t reg_id) {
	state->cpu_flags_register = reg_id;
	return;
}

extern "C"
void simunicorn_set_register_blacklist(State *state, uint64_t *reg_list, uint64_t count) {
	state->blacklisted_registers.clear();
	for (auto i = 0; i < count; i++) {
		state->blacklisted_registers.emplace(reg_list[i]);
	}
	return;
}

// VEX re-execution data

extern "C"
uint64_t simunicorn_get_count_of_blocks_with_symbolic_instrs(State *state) {
	return state->block_details_to_return.size();
}

extern "C"
void simunicorn_get_details_of_blocks_with_symbolic_instrs(State *state, sym_block_details_ret_t *ret_block_details) {
	for (auto i = 0; i < state->block_details_to_return.size(); i++) {
		ret_block_details[i].block_addr = state->block_details_to_return[i].block_addr;
		ret_block_details[i].block_size = state->block_details_to_return[i].block_size;
		ret_block_details[i].symbolic_instrs = &(state->block_details_to_return[i].symbolic_instrs[0]);
		ret_block_details[i].symbolic_instrs_count = state->block_details_to_return[i].symbolic_instrs.size();
		ret_block_details[i].register_values = &(state->block_details_to_return[i].register_values[0]);
		ret_block_details[i].register_values_count = state->block_details_to_return[i].register_values.size();
	}
	return;
}
