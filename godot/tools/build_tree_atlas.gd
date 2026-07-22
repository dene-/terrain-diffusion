@tool
extends SceneTree

const CELL_WIDTH := 512
const ATLAS_HEIGHT := 1024
const SOURCE_FILES := [
	"spruce-v3.png",
	"birch.png",
	"oak.png",
	"acacia.png",
	"tropical.png",
]
const OUTPUT_PATH := "res://assets/vegetation/tree_atlas.webp"


func _init() -> void:
	var atlas := Image.create_empty(CELL_WIDTH * SOURCE_FILES.size(), ATLAS_HEIGHT, false, Image.FORMAT_RGBA8)
	atlas.fill(Color.TRANSPARENT)

	for species in SOURCE_FILES.size():
		var source := Image.new()
		var source_path := ProjectSettings.globalize_path("res://../web/public/assets/%s" % SOURCE_FILES[species])
		var load_error := source.load(source_path)
		if load_error != OK or source.is_empty():
			push_error("Could not load vegetation source: %s" % source_path)
			quit(1)
			return
		source.convert(Image.FORMAT_RGBA8)
		var scale := minf(
			float(CELL_WIDTH - 24) / float(source.get_width()),
			float(ATLAS_HEIGHT - 16) / float(source.get_height()))
		var width := maxi(1, roundi(float(source.get_width()) * scale))
		var height := maxi(1, roundi(float(source.get_height()) * scale))
		source.resize(width, height, Image.INTERPOLATE_LANCZOS)
		var destination := Vector2i(
			species * CELL_WIDTH + (CELL_WIDTH - width) / 2,
			ATLAS_HEIGHT - height - 4)
		atlas.blit_rect(source, Rect2i(Vector2i.ZERO, Vector2i(width, height)), destination)

	var output_directory := ProjectSettings.globalize_path("res://assets/vegetation")
	var directory_error := DirAccess.make_dir_recursive_absolute(output_directory)
	if directory_error != OK:
		push_error("Could not create vegetation asset directory: %s" % output_directory)
		quit(1)
		return
	var save_error := atlas.save_webp(ProjectSettings.globalize_path(OUTPUT_PATH), false)
	if save_error != OK:
		push_error("Could not save tree atlas: %s" % OUTPUT_PATH)
		quit(1)
		return
	print("Generated %s (%dx%d)" % [OUTPUT_PATH, atlas.get_width(), atlas.get_height()])
	quit()
