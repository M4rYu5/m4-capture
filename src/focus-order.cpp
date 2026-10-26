#include <obs.hpp>

#include <vector>

bool raise_source_items(obs_scene_t *scene, obs_source_t *source)
{
	// Snapshot before reordering: enumeration traverses the same linked list.
	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(
	    scene,
	    [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
		    static_cast<std::vector<OBSSceneItem> *>(data)->emplace_back(item);
		    return true;
	    },
	    &items);
	bool changed = false;
	for (const auto &item : items) {
		if (obs_sceneitem_is_group(item))
			changed |= raise_source_items(obs_sceneitem_group_get_scene(item), source);
		if (obs_sceneitem_get_source(item) == source &&
		    obs_sceneitem_get_order_position(item) != int(items.size()) - 1) {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
			changed = true;
		}
	}
	return changed;
}
