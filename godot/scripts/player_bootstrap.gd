extends CharacterBody3D

var _activation_attempts := 0


func _ready() -> void:
	call_deferred("_activate_native_controller")


func _activate_native_controller() -> void:
	_activation_attempts += 1
	if not ClassDB.class_exists("PlayerController"):
		if _activation_attempts < 120:
			call_deferred("_activate_native_controller")
			return
		push_error("PlayerController GDExtension class is unavailable; rebuild the extension and restart Godot")
		return

	var controller := ClassDB.instantiate("PlayerController") as CharacterBody3D
	if controller == null:
		push_error("Could not instantiate PlayerController")
		return

	controller.name = name
	controller.transform = transform
	controller.collision_layer = collision_layer
	controller.collision_mask = collision_mask

	for child: Node in get_children():
		remove_child(child)
		controller.add_child(child)

	var parent_node := get_parent()
	name = &"PlayerBootstrapRetired"
	parent_node.add_child(controller)
	queue_free()
