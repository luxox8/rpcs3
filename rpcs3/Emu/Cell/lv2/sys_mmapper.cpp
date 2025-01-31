﻿#include "stdafx.h"
#include "sys_mmapper.h"

#include "Emu/Cell/PPUThread.h"
#include "sys_ppu_thread.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Memory/vm_var.h"
#include "Utilities/VirtualMemory.h"
#include "sys_memory.h"

LOG_CHANNEL(sys_mmapper);

lv2_memory::lv2_memory(u32 size, u32 align, u64 flags, const std::shared_ptr<lv2_memory_container>& ct)
	: size(size)
	, align(align)
	, flags(flags)
	, ct(ct)
	, shm(std::make_shared<utils::shm>(size))
{
}

error_code sys_mmapper_allocate_address(u64 size, u64 flags, u64 alignment, vm::ptr<u32> alloc_addr)
{
	sys_mmapper.error("sys_mmapper_allocate_address(size=0x%llx, flags=0x%llx, alignment=0x%llx, alloc_addr=*0x%x)", size, flags, alignment, alloc_addr);

	if (size % 0x10000000)
	{
		return CELL_EALIGN;
	}

	if (size > UINT32_MAX)
	{
		return CELL_ENOMEM;
	}

	// This is a workaround for psl1ght, which gives us an alignment of 0, which is technically invalid, but apparently is allowed on actual ps3
	// https://github.com/ps3dev/PSL1GHT/blob/534e58950732c54dc6a553910b653c99ba6e9edc/ppu/librt/sbrk.c#L71
	if (!alignment)
	{
		alignment = 0x10000000;
	}

	switch (alignment)
	{
	case 0x10000000:
	case 0x20000000:
	case 0x40000000:
	case 0x80000000:
	{
		if (const auto area = vm::find_map(static_cast<u32>(size), static_cast<u32>(alignment), flags & SYS_MEMORY_PAGE_SIZE_MASK))
		{
			*alloc_addr = area->addr;
			return CELL_OK;
		}

		return CELL_ENOMEM;
	}
	}

	return CELL_EALIGN;
}

error_code sys_mmapper_allocate_fixed_address()
{
	sys_mmapper.error("sys_mmapper_allocate_fixed_address()");

	if (!vm::map(0xB0000000, 0x10000000, SYS_MEMORY_PAGE_SIZE_1M))
	{
		return CELL_EEXIST;
	}

	return CELL_OK;
}

error_code sys_mmapper_allocate_shared_memory(u64 unk, u32 size, u64 flags, vm::ptr<u32> mem_id)
{
	sys_mmapper.warning("sys_mmapper_allocate_shared_memory(0x%llx, size=0x%x, flags=0x%llx, mem_id=*0x%x)", unk, size, flags, mem_id);

	// Check page granularity
	switch (flags & SYS_MEMORY_PAGE_SIZE_MASK)
	{
	case 0:
	case SYS_MEMORY_PAGE_SIZE_1M:
	{
		if (size % 0x100000)
		{
			return CELL_EALIGN;
		}

		break;
	}

	case SYS_MEMORY_PAGE_SIZE_64K:
	{
		if (size % 0x10000)
		{
			return CELL_EALIGN;
		}

		break;
	}

	default:
	{
		return CELL_EINVAL;
	}
	}

	// Get "default" memory container
	const auto dct = fxm::get<lv2_memory_container>();

	if (!dct->take(size))
	{
		return CELL_ENOMEM;
	}

	// Generate a new mem ID
	*mem_id = idm::make<lv2_obj, lv2_memory>(size, flags & SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : 0x100000, flags, dct);

	return CELL_OK;
}

error_code sys_mmapper_allocate_shared_memory_from_container(u64 unk, u32 size, u32 cid, u64 flags, vm::ptr<u32> mem_id)
{
	sys_mmapper.error("sys_mmapper_allocate_shared_memory_from_container(0x%llx, size=0x%x, cid=0x%x, flags=0x%llx, mem_id=*0x%x)", unk, size, cid, flags, mem_id);

	// Check page granularity.
	switch (flags & SYS_MEMORY_PAGE_SIZE_MASK)
	{
	case 0:
	case SYS_MEMORY_PAGE_SIZE_1M:
	{
		if (size % 0x100000)
		{
			return CELL_EALIGN;
		}

		break;
	}

	case SYS_MEMORY_PAGE_SIZE_64K:
	{
		if (size % 0x10000)
		{
			return CELL_EALIGN;
		}

		break;
	}

	default:
	{
		return CELL_EINVAL;
	}
	}

	const auto ct = idm::get<lv2_memory_container>(cid, [&](lv2_memory_container& ct) -> CellError
	{
		// Try to get "physical memory"
		if (!ct.take(size))
		{
			return CELL_ENOMEM;
		}

		return {};
	});

	if (!ct)
	{
		return CELL_ESRCH;
	}

	if (ct.ret)
	{
		return ct.ret;
	}

	// Generate a new mem ID
	*mem_id = idm::make<lv2_obj, lv2_memory>(size, flags & SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : 0x100000, flags, ct.ptr);

	return CELL_OK;
}

error_code sys_mmapper_change_address_access_right(u32 addr, u64 flags)
{
	sys_mmapper.todo("sys_mmapper_change_address_access_right(addr=0x%x, flags=0x%llx)", addr, flags);

	return CELL_OK;
}

error_code sys_mmapper_free_address(u32 addr)
{
	sys_mmapper.error("sys_mmapper_free_address(addr=0x%x)", addr);

	if (addr < 0x20000000 || addr >= 0xC0000000)
	{
		return {CELL_EINVAL, addr};
	}

	// If page fault notify exists and an address in this area is faulted, we can't free the memory.
	auto pf_events = fxm::get_always<page_fault_event_entries>();
	std::lock_guard pf_lock(pf_events->pf_mutex);

	for (const auto& ev : pf_events->events)
	{
		auto mem = vm::get(vm::any, addr);
		if (mem && addr <= ev.second && ev.second <= addr + mem->size - 1)
		{
			return CELL_EBUSY;
		}
	}

	// Try to unmap area
	const auto area = vm::unmap(addr, true);

	if (!area)
	{
		return {CELL_EINVAL, addr};
	}

	if (area.use_count() != 1)
	{
		return CELL_EBUSY;
	}

	// If a memory block is freed, remove it from page notification table.
	auto pf_entries = fxm::get_always<page_fault_notification_entries>();
	std::lock_guard lock(pf_entries->mutex);

	auto ind_to_remove = pf_entries->entries.begin();
	for (; ind_to_remove != pf_entries->entries.end(); ++ind_to_remove)
	{
		if (addr == ind_to_remove->start_addr)
		{
			break;
		}
	}
	if (ind_to_remove != pf_entries->entries.end())
	{
		pf_entries->entries.erase(ind_to_remove);
	}

	return CELL_OK;
}

error_code sys_mmapper_free_shared_memory(u32 mem_id)
{
	sys_mmapper.warning("sys_mmapper_free_shared_memory(mem_id=0x%x)", mem_id);

	// Conditionally remove memory ID
	const auto mem = idm::withdraw<lv2_obj, lv2_memory>(mem_id, [&](lv2_memory& mem) -> CellError
	{
		if (mem.counter)
		{
			return CELL_EBUSY;
		}

		return {};
	});

	if (!mem)
	{
		return CELL_ESRCH;
	}

	if (mem.ret)
	{
		return mem.ret;
	}

	// Return "physical memory" to the memory container
	mem->ct->used -= mem->size;

	return CELL_OK;
}

error_code sys_mmapper_map_shared_memory(u32 addr, u32 mem_id, u64 flags)
{
	sys_mmapper.warning("sys_mmapper_map_shared_memory(addr=0x%x, mem_id=0x%x, flags=0x%llx)", addr, mem_id, flags);

	const auto area = vm::get(vm::any, addr);

	if (!area || addr < 0x20000000 || addr >= 0xC0000000)
	{
		return CELL_EINVAL;
	}

	const auto mem = idm::get<lv2_obj, lv2_memory>(mem_id, [&](lv2_memory& mem) -> CellError
	{
		const u32 page_alignment = area->flags & SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : 0x100000;

		if (mem.align < page_alignment)
		{
			return CELL_EINVAL;
		}

		if (addr % page_alignment)
		{
			return CELL_EALIGN;
		}

		mem.counter++;
		return {};
	});

	if (!mem)
	{
		return CELL_ESRCH;
	}

	if (mem.ret)
	{
		return mem.ret;
	}

	if (!area->falloc(addr, mem->size, &mem->shm, mem->align == 0x10000 ? SYS_MEMORY_PAGE_SIZE_64K : SYS_MEMORY_PAGE_SIZE_1M))
	{
		mem->counter--;
		return CELL_EBUSY;
	}

	return CELL_OK;
}

error_code sys_mmapper_search_and_map(u32 start_addr, u32 mem_id, u64 flags, vm::ptr<u32> alloc_addr)
{
	sys_mmapper.warning("sys_mmapper_search_and_map(start_addr=0x%x, mem_id=0x%x, flags=0x%llx, alloc_addr=*0x%x)", start_addr, mem_id, flags, alloc_addr);

	const auto area = vm::get(vm::any, start_addr);

	if (!area || start_addr < 0x20000000 || start_addr >= 0xC0000000)
	{
		return {CELL_EINVAL, start_addr};
	}

	const auto mem = idm::get<lv2_obj, lv2_memory>(mem_id, [&](lv2_memory& mem)
	{
		mem.counter++;
	});

	if (!mem)
	{
		return CELL_ESRCH;
	}

	const u32 addr = area->alloc(mem->size, mem->align, &mem->shm, mem->align == 0x10000 ? SYS_MEMORY_PAGE_SIZE_64K : SYS_MEMORY_PAGE_SIZE_1M);

	if (!addr)
	{
		mem->counter--;
		return CELL_ENOMEM;
	}

	*alloc_addr = addr;
	return CELL_OK;
}

error_code sys_mmapper_unmap_shared_memory(u32 addr, vm::ptr<u32> mem_id)
{
	sys_mmapper.warning("sys_mmapper_unmap_shared_memory(addr=0x%x, mem_id=*0x%x)", addr, mem_id);

	const auto area = vm::get(vm::any, addr);

	if (!area || addr < 0x20000000 || addr >= 0xC0000000)
	{
		return {CELL_EINVAL, addr};
	}

	const auto shm = area->get(addr);

	if (!shm.second)
	{
		return {CELL_EINVAL, addr};
	}

	const auto mem = idm::select<lv2_obj, lv2_memory>([&](u32 id, lv2_memory& mem) -> u32
	{
		if (mem.shm.get() == shm.second.get())
		{
			return id;
		}

		return 0;
	});

	if (!mem)
	{
		return {CELL_EINVAL, addr};
	}

	if (!area->dealloc(addr, &shm.second))
	{
		return {CELL_EINVAL, addr};
	}

	// Write out the ID
	*mem_id = mem.ret;

	// Acknowledge
	mem->counter--;

	return CELL_OK;
}

error_code sys_mmapper_enable_page_fault_notification(u32 start_addr, u32 event_queue_id)
{
	sys_mmapper.warning("sys_mmapper_enable_page_fault_notification(start_addr=0x%x, event_queue_id=0x%x)", start_addr, event_queue_id);

	auto mem = vm::get(vm::any, start_addr);
	if (!mem || start_addr != mem->addr || start_addr < 0x20000000 || start_addr >= 0xC0000000)
	{
		return {CELL_EINVAL, start_addr};
	}

	// TODO: Check memory region's flags to make sure the memory can be used for page faults.

	auto queue = idm::get<lv2_obj, lv2_event_queue>(event_queue_id);

	if (!queue)
	{ // Can't connect the queue if it doesn't exist.
		return CELL_ESRCH;
	}

	vm::var<u32> port_id(0);
	error_code res = sys_event_port_create(port_id, SYS_EVENT_PORT_LOCAL, SYS_MEMORY_PAGE_FAULT_EVENT_KEY);
	sys_event_port_connect_local(port_id->value(), event_queue_id);

	if (res == CELL_EAGAIN)
	{ // Not enough system resources.
		return CELL_EAGAIN;
	}

	auto pf_entries = fxm::get_always<page_fault_notification_entries>();
	std::unique_lock lock(pf_entries->mutex);

	// Return error code if page fault notifications are already enabled
	for (const auto& entry : pf_entries->entries)
	{
		if (entry.start_addr == start_addr)
		{
			lock.unlock();
			sys_event_port_disconnect(port_id->value());
			sys_event_port_destroy(port_id->value());
			return CELL_EBUSY;
		}
	}

	page_fault_notification_entry entry{ start_addr, event_queue_id, port_id->value() };
	pf_entries->entries.emplace_back(entry);

	return CELL_OK;
}

CellError mmapper_thread_recover_page_fault(u32 id)
{
	// We can only wake a thread if it is being suspended for a page fault.
	auto pf_events = fxm::get_always<page_fault_event_entries>();
	{
		std::lock_guard pf_lock(pf_events->pf_mutex);
		auto pf_event_ind = pf_events->events.find(id);

		if (pf_event_ind == pf_events->events.end())
		{
			// if not found...
			return CELL_EINVAL;
		}

		pf_events->events.erase(pf_event_ind);
	}

	pf_events->cond.notify_all();
	return CellError(CELL_OK);
}
