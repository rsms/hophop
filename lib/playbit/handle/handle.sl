import "platform/playbit" { Handle as PBHandle, HandleInfo as PBHandleInfo, handle_close, handle_list }

pub type Handle i32

pub const FilterByName u64 = 1

pub const FilterByObjectId u64 = 2

pub const FilterByObjectType u64 = 3

pub fn close(handle Handle) i32 {
	return handle_close(handle as PBHandle) as i32
}

pub fn find_by_name(name u16) Handle {
	var infos [PBHandleInfo 4]
	var count = handle_list(&infos[0], handlesCap: len(infos) as u32, flags: 1, predicate: name as u64)
	if count <= 0 {
		return count as Handle
	}
	return infos[0].handle as Handle
}

pub fn find_by_object_type(objectType u16) Handle {
	var infos [PBHandleInfo 8]
	var count = handle_list(&infos[0], handlesCap: len(infos) as u32, flags: 3, predicate: objectType as u64)
	if count <= 0 {
		return count as Handle
	}
	return infos[0].handle as Handle
}
