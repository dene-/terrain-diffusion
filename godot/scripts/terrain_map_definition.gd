@tool
class_name TerrainMapDefinition
extends Resource

@export_category("Identity")
@export var map_id: String = ""
@export var display_name: String = ""

@export_category("Runtime")
@export var archive_path: String = ""

@export_category("Editor Preview")
@export var editor_heightmap_path: String = ""
@export var editor_albedo_path: String = ""
@export_range(1.0, 2000000.0, 1.0, "suffix:m") var width_m := 614400.0
@export_range(1.0, 2000000.0, 1.0, "suffix:m") var height_m := 614400.0
@export var elevation_offset := 32768.0
@export var elevation_units_per_m := 2.0
