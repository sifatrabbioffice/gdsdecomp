#include "gdre_settings.h"

#include "bytecode/bytecode_base.h"
#include "bytecode/bytecode_tester.h"
#include "compat/config_file_compat.h"
#include "compat/resource_compat_binary.h"
#include "compat/resource_loader_compat.h"
#include "compat/variant_writer_compat.h"
#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"
#include "crypto/custom_decryptor.h"
#include "exporters/translation_exporter.h"
#include "main/main.h"
#include "modules/gdscript/gdscript_resource_format.h"
#include "modules/zip/zip_reader.h"
#include "plugin_manager/plugin_manager.h"
#include "utility/app_version_getter.h"
#include "utility/common.h"
#include "utility/file_access_buffer.h"
#include "utility/file_access_gdre.h"
#include "utility/gdre_logger.h"
#include "utility/gdre_packed_source.h"
#include "utility/gdre_version.gen.h"
#include "utility/glob.h"
#include "utility/godot_mono_decomp_wrapper.h"
#include "utility/import_info.h"
#include "utility/pcfg_loader.h"
#include "utility/task_manager.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/object/script_language.h"
#include "core/string/translation.h"
#include "modules/regex/regex.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#endif

#include <sys/types.h>

#if defined(WINDOWS_ENABLED)
#include <windows.h>
#elif defined(UNIX_ENABLED)
#include <limits.h>
#include <unistd.h>
#endif
#include <stdlib.h>

#ifdef ANDROID_ENABLED
// #include "drivers/gles3/shader_gles3.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#endif

String GDRESettings::_get_cwd() {
#if defined(WINDOWS_ENABLED)
	const DWORD expected_size = ::GetCurrentDirectoryW(0, nullptr);

	Char16String buffer;
	buffer.resize_uninitialized((int)expected_size);
	if (::GetCurrentDirectoryW(expected_size, (wchar_t *)buffer.ptrw()) == 0)
		return ".";

	String result;
	if (result.append_utf16(buffer.ptr())) {
		return ".";
	}
	return result.simplify_path();
#elif defined(UNIX_ENABLED)
	char buffer[PATH_MAX];
	if (::getcwd(buffer, sizeof(buffer)) == nullptr) {
		return ".";
	}

	String result;
	if (result.append_utf8(buffer)) {
		return ".";
	}

	return result.simplify_path();
#else
	return ".";
#endif
}

GDRESettings *GDRESettings::singleton = nullptr;

bool GDRESettings::check_if_dir_is_v4() {
	// these are files that will only show up in version 4
	static const Vector<String> wildcards = { "*.ctex" };
	if (get_file_list(wildcards).size() > 0) {
		return true;
	} else {
		return false;
	}
}

bool GDRESettings::check_if_dir_is_v3() {
	// these are files that will only show up in version 3
	static const Vector<String> wildcards = { "*.stex" };
	if (get_file_list(wildcards).size() > 0) {
		return true;
	} else {
		return false;
	}
}

bool GDRESettings::check_if_dir_is_v2() {
	// these are files that will NOT show up in version 2
	static const Vector<String> wildcards = { "*.import", "*.remap" };
	if (get_file_list(wildcards).size() == 0) {
		return true;
	} else {
		return false;
	}
}

int GDRESettings::get_ver_major_from_dir() {
	if (check_if_dir_is_v2() && (FileAccess::exists("res://engine.cfb") || FileAccess::exists("res://engine.cfg"))) {
		return 2;
	}
	if (check_if_dir_is_v4()) {
		return 4;
	}
	if (check_if_dir_is_v3()) {
		return 3;
	}
	bool not_v2 = !check_if_dir_is_v2() || FileAccess::exists("res://project.binary") || FileAccess::exists("res://project.godot");

	// deeper checking; we know it's not v2, so we don't need to check that.
	HashSet<String> v2exts;
	HashSet<String> v3exts;
	HashSet<String> v4exts;
	HashSet<String> v2onlyexts;
	HashSet<String> v3onlyexts;
	HashSet<String> v4onlyexts;

	auto get_exts_func([&](HashSet<String> &ext, HashSet<String> &ext2, int ver_major) {
		List<String> exts;
		ResourceCompatLoader::get_base_extensions(&exts, ver_major);
		for (const String &extf : exts) {
			ext.insert(extf);
			ext2.insert(extf);
		}
	});
	get_exts_func(v2onlyexts, v2exts, 2);
	get_exts_func(v3onlyexts, v3exts, 3);
	get_exts_func(v4onlyexts, v4exts, 4);
	auto check_func([&](HashSet<String> &exts) {
		Vector<String> wildcards;
		for (auto &ext : exts) {
			wildcards.push_back("*." + ext);
		}
		auto list = get_file_list(wildcards);
		if (list.size() > 0) {
			return true;
		}
		return false;
	});

	for (const String &ext : v2exts) {
		if (v4exts.has(ext) || v3exts.has(ext)) {
			v4onlyexts.erase(ext);
			v3onlyexts.erase(ext);
			v2onlyexts.erase(ext);
		}
	}

	for (const String &ext : v3exts) {
		if (v4exts.has(ext)) {
			v4onlyexts.erase(ext);
			v3onlyexts.erase(ext);
		}
	}
	if (check_func(v4onlyexts)) {
		return 4;
	}
	if (check_func(v3onlyexts)) {
		return 3;
	}
	if (!not_v2) {
		if (check_func(v2onlyexts)) {
			return 2;
		}
	}
	return 0;
}

String GDRESettings::exec_dir = GDRESettings::_get_cwd();
GDRESettings *GDRESettings::get_singleton() {
	return singleton;
}
void addCompatibilityClasses() {
	ClassDB::add_compatibility_class("PHashTranslation", "OptimizedTranslation");
}

GDRESettings::GDRESettings() {
#ifdef TOOLS_ENABLED
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->set_warn_on_surface_upgrade(false);
	}
#endif
	singleton = this;
	addCompatibilityClasses();
#ifdef TOOLS_ENABLED
	print_line("GDRE User path: " + get_gdre_user_path());
#endif

	gdre_resource_path = ProjectSettings::get_singleton()->get_resource_path();
	logger = memnew(GDRELogger);
	headless = !RenderingServer::get_singleton() || RenderingServer::get_singleton()->get_video_adapter_name().is_empty();
	add_logger();
	PluginManager::load_cache();
#ifdef ANDROID_ENABLED
	if (!OS::get_singleton()->request_permission("android.permission.READ_EXTERNAL_STORAGE")) {
		ERR_PRINT("Permission READ_EXTERNAL_STORAGE is required to access external storage!");
	}
	if (!OS::get_singleton()->request_permission("android.permission.WRITE_EXTERNAL_STORAGE")) {
		ERR_PRINT("Permission WRITE_EXTERNAL_STORAGE is required to access external storage!");
	}
	if (!OS::get_singleton()->request_permission("android.permission.MANAGE_EXTERNAL_STORAGE")) {
		ERR_PRINT("Permission WRITE_EXTERNAL_STORAGE is required to access external storage!");
	}
	String old_shader_cache_user_dir = ShaderRD::get_shader_cache_user_dir();
	String new_shader_cache_user_dir = get_gdre_user_path().path_join("shader_cache");
	print_line("old shader cache user dir: " + old_shader_cache_user_dir);
	print_line("new shader cache user dir: " + new_shader_cache_user_dir);
	gdre::ensure_dir(new_shader_cache_user_dir);
	ShaderRD::set_shader_cache_user_dir(new_shader_cache_user_dir);
#endif
}

GDRESettings::~GDRESettings() {
	PluginManager::save_cache();
	remove_current_pack();
	singleton = nullptr;
	logger->_disable();
}

String GDRESettings::get_cwd() {
	return GDRESettings::_get_cwd();
}

void GDRESettings::set_exec_dir(const String &p_cwd) {
	GDRESettings::exec_dir = p_cwd;
}

String GDRESettings::get_exec_dir() {
	return GDRESettings::exec_dir;
}

bool GDRESettings::are_imports_loaded() const {
	return import_files.size() > 0;
}

String GDRESettings::get_gdre_resource_path() const {
	return gdre_resource_path;
}

String GDRESettings::get_gdre_tmp_path() {
	return get_gdre_user_path().path_join(".tmp");
}

String GDRESettings::get_gdre_user_path() {
	String gdre_user_path = OS::get_singleton()->get_user_data_dir();
	if (gdre_user_path.contains("[unnamed project]")) {
		gdre_user_path = gdre_user_path.replace("[unnamed project]", "gdre_tests");
	}
	return gdre_user_path;
}

bool GDRESettings::is_pack_loaded() const {
	return current_project.is_valid();
}

bool GDRESettings::has_valid_version() const {
	return is_pack_loaded() && current_project->version.is_valid() && current_project->version->is_valid_semver();
}

PackInfo::PackType GDRESettings::get_pack_type() const {
	return is_pack_loaded() ? current_project->type : PackInfo::UNKNOWN;
}
String GDRESettings::get_pack_path() const {
	return is_pack_loaded() ? current_project->pack_file : "";
}

String GDRESettings::get_version_string() const {
	return has_valid_version() ? current_project->version->as_text() : String();
}
uint32_t GDRESettings::get_ver_major() const {
	return has_valid_version() ? current_project->version->get_major() : 0;
}
uint32_t GDRESettings::get_ver_minor() const {
	return has_valid_version() ? current_project->version->get_minor() : 0;
}
uint32_t GDRESettings::get_ver_rev() const {
	return has_valid_version() ? current_project->version->get_patch() : 0;
}

uint32_t GDRESettings::get_file_count() const {
	if (!is_pack_loaded()) {
		return 0;
	}
	int count = 0;
	for (const auto &pack : packs) {
		count += pack->file_count;
	}
	return count;
}

bool GDRESettings::uses_nonstandard_headers() const {
	if (!is_pack_loaded()) {
		return false;
	}
	return !current_project->non_standard_header.is_empty();
}

String GDRESettings::get_non_standard_header() const {
	if (!is_pack_loaded()) {
		return "";
	}
	return current_project->non_standard_header;
}

void GDRESettings::set_project_path(const String &p_path) {
	project_path = p_path;
}
String GDRESettings::get_project_path() const {
	return project_path;
}
bool GDRESettings::is_project_config_loaded() const {
	if (!is_pack_loaded()) {
		return false;
	}
	bool is_loaded = current_project->pcfg->is_loaded();
	return is_loaded;
}

void GDRESettings::remove_current_pack() {
	if (current_project.is_valid() && !current_project->assembly_temp_dir.is_empty()) {
		gdre::rimraf(current_project->assembly_temp_dir);
		current_project->assembly_temp_dir = "";
	}
	current_project = Ref<ProjectInfo>();
	packs.clear();
	import_files.clear();
	remap_iinfo.clear();
}

String get_standalone_pck_path() {
	String exec_path = OS::get_singleton()->get_executable_path();
	String exec_dir = exec_path.get_base_dir();
	String exec_filename = exec_path.get_file();
	String exec_basename = exec_filename.get_basename();

	return exec_dir.path_join(exec_basename + ".pck");
}

Error GDRESettings::load_dir(const String &p_path) {
	Ref<DirAccess> da = DirAccess::open(p_path.get_base_dir());
	ERR_FAIL_COND_V_MSG(da.is_null(), ERR_FILE_CANT_OPEN, "FATAL ERROR: Can't find folder!");
	ERR_FAIL_COND_V_MSG(!da->dir_exists(p_path), ERR_FILE_CANT_OPEN, "FATAL ERROR: Can't find folder!");

	Error err = GDREPackedData::get_singleton()->add_dir(p_path, false);
	if (err != OK) {
		ERR_FAIL_V_MSG(err, "FATAL ERROR: Can't open directory!");
	}
	String sparse_pck_path = p_path.path_join("assets.sparsepck");
	if (FileAccess::exists(sparse_pck_path)) {
		for (const auto &pack : packs) {
			if (pack->pack_file == sparse_pck_path) {
				return OK;
			}
		}
		print_line("Checking if we need to load detected sparse pack...");
		bool needs_load = false;
		if (pack_has_project_config()) {
			String proj_config_path = p_path.path_join(has_path_loaded("res://project.binary") ? "project.binary" : "project.godot");
			ProjectConfigLoader pcfg_loader;
			if (pcfg_loader.load_cfb(proj_config_path, 4, 5) != OK) {
				needs_load = true;
			}
		} else {
			needs_load = true;
		}

		if (needs_load) {
			print_line("Loading detected sparse pack...");
			err = GDREPackedData::get_singleton()->add_pack(sparse_pck_path, true, 0);
			if (err != OK) {
				if (error_encryption) {
					ERR_FAIL_V_MSG(err, "FATAL ERROR: Can't open detected sparse pack! (Did you set the correct key?)");
				}
				ERR_FAIL_V_MSG(err, "FATAL ERROR: Can't open detected sparse pack!");
			}
			print_line("Loaded detected sparse pack!");
		} else {
			print_line("Skipping loading detected sparse pack...");
		}
	}
	return OK;
}

namespace {
bool is_executable(const String &p_path) {
	String extension = p_path.get_extension().to_lower();
	if (extension == "exe") {
		return true;
	}
	if (extension == "pck" || extension == "apk" || extension == "zip" || extension == "ipa") {
		return false;
	}
	return GDREPackedSource::is_executable(p_path);
}
} //namespace

Error GDRESettings::load_pck(const String &p_path) {
	for (const auto &pack : packs) {
		if (pack->pack_file == p_path) {
			return ERR_ALREADY_IN_USE;
		}
	}
	Error err = GDREPackedData::get_singleton()->add_pack(p_path, true, 0);
	if (err) {
		ERR_FAIL_COND_V_MSG(error_encryption, ERR_PRINTER_ON_FIRE, "FATAL ERROR: Cannot open encrypted pck! (wrong key?)");
	}
	ERR_FAIL_COND_V_MSG(err, err, "FATAL ERROR: Can't open pack!");
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), ERR_FILE_CANT_READ, "FATAL ERROR: loaded project pack, but didn't load files from it!");
	return OK;
}

bool is_zip_file_pack(const String &p_path) {
	Ref<ZIPReader> zip = memnew(ZIPReader);
	Error err = zip->open(p_path);
	if (err) {
		return false;
	}
	auto files = zip->get_files();
	for (int i = 0; i < files.size(); i++) {
		if (files[i] == "engine.cfg" || files[i] == "engine.cfb" || files[i] == "project.godot" || files[i] == "project.binary") {
			return true;
		}
		if (files[i].begins_with(".godot/")) {
			return true;
		}
	}
	return false;
}
String GDRESettings::get_home_dir() {
#ifdef WINDOWS_ENABLED
	return OS::get_singleton()->get_environment("USERPROFILE");
#else
	return OS::get_singleton()->get_environment("HOME");
#endif
}
String GDRESettings::sanitize_home_in_path(const String &p_path) {
#ifdef WINDOWS_ENABLED
	String home_dir = OS::get_singleton()->get_environment("USERPROFILE");
#else
	String home_dir = OS::get_singleton()->get_environment("HOME");
#endif
	if (p_path.begins_with(home_dir)) {
		return String("~").path_join(p_path.replace_first(home_dir, ""));
	}
	return p_path;
}
namespace {
bool is_godot_directory(const String &p_path) {
	static const Vector<String> godot_files = { "project.godot", "project.binary", ".godot", ".import", "engine.cfg", "engine.cfb" };
	return gdre::directory_has_any_of(p_path, godot_files);
}
} //namespace

struct FileModifiedTimeSorter {
	bool operator()(const String &p_a, const String &p_b) const {
		return FileAccess::get_modified_time(p_a) < FileAccess::get_modified_time(p_b);
	}
};

Vector<String> GDRESettings::sort_and_validate_pck_files(const Vector<String> &p_paths) {
	Vector<String> pck_files;
	String main_pck_path;
	Vector<String> additional_main_pck_paths;

	size_t dir_count = 0;

	for (int i = 0; i < p_paths.size(); i++) {
		String path = gdre::get_full_path(p_paths[i]);
		String ext = path.get_extension().to_lower();
		if (DirAccess::exists(path)) {
			if (ext == "app") {
				String resources_path = path.path_join("Contents").path_join("Resources");
				if (!DirAccess::exists(resources_path)) {
					WARN_PRINT("Contents/Resources directory not found in .app bundle, searching for pck in root...");
					resources_path = path;
				}
				auto list = gdre::get_recursive_dir_list(resources_path, { "*.pck" }, true);
				if (list.is_empty() && resources_path != path) {
					WARN_PRINT("Can't find pck file in Contents/Resources, searching for pck in root...");
					list = gdre::get_recursive_dir_list(path, { "*.pck" }, true);
				}
				ERR_CONTINUE_MSG(list.is_empty(), "Can't find pck file in .app bundle!");
				String gamename = path.get_file().get_basename();
				Vector<String> new_list;
				for (auto &pck : list) {
					if (gamename.filenocasecmp_to(pck.get_file().get_basename()) == 0) {
						main_pck_path = pck;
					} else {
						new_list.push_back(pck);
					}
				}
				if (main_pck_path.is_empty()) {
					main_pck_path = new_list[0];
					new_list.remove_at(0);
					additional_main_pck_paths = new_list;
				} else {
					pck_files.append_array(new_list);
				}
				continue;
			}
			if (dir_count > 1) {
				ERR_FAIL_V_MSG({}, "Cannot specify multiple directories!");
			}
			if (!is_godot_directory(path)) {
				ERR_CONTINUE_MSG(true, "Not a Godot directory: " + sanitize_home_in_path(path));
			}
			dir_count++;
			if (!main_pck_path.is_empty()) {
				pck_files.push_back(main_pck_path);
			}
			main_pck_path = path;
		} else if (ext == "apk" || ext == "ipa") {
			if (!main_pck_path.is_empty()) {
				main_pck_path = path;
			} else {
				pck_files.push_back(path);
			}
		} else if (is_executable(path)) {
			if (GDREPackedSource::has_embedded_pck(path)) {
				if (main_pck_path.is_empty()) {
					main_pck_path = path;
				} else {
					pck_files.push_back(path);
				}
				continue;
			}
			auto san_path = sanitize_home_in_path(path);
			String new_path = path;
			String parent_path = path.get_base_dir();
			if (parent_path.is_empty()) {
				parent_path = GDRESettings::get_exec_dir();
			}
			Error e = ERR_FILE_NOT_FOUND;
			if (parent_path.get_file().to_lower() == "macos") {
				parent_path = parent_path.get_base_dir().path_join("Resources");
				String pck_path = parent_path.path_join(path.get_file().get_basename() + ".pck");
				if (FileAccess::exists(pck_path)) {
					new_path = pck_path;
					e = OK;
				}
				if (pck_files.has(new_path)) {
					WARN_PRINT("EXE does not have an embedded pck, not loading " + san_path);
					continue;
				}
			}
			if (e != OK) {
				String pck_path = path.get_basename() + ".pck";
				bool only_1_path = pck_files.size() == 1;
				bool already_has_path = pck_files.has(pck_path);
				bool exists = FileAccess::exists(pck_path);
				if (!only_1_path && (already_has_path || !exists)) {
					WARN_PRINT("EXE does not have an embedded pck, not loading " + san_path);
					continue;
				}
				ERR_FAIL_COND_V_MSG(!exists, {}, "Can't find embedded pck file in executable and cannot find pck file in same directory!");
				new_path = pck_path;
			}
			path = new_path;
			WARN_PRINT("Could not find embedded pck in EXE, found pck file, loading from: " + path);
			if (main_pck_path.is_empty()) {
				main_pck_path = path;
			} else {
				pck_files.push_back(path);
			}
		} else {
			pck_files.push_back(path);
		}
	}

	if (main_pck_path.is_empty() && pck_files.size() > 1) {
		HashMap<String, Vector<String>> common_base_dirs;
		for (int i = 0; i < pck_files.size(); i++) {
			String path = pck_files[i];
			String base_dir = path.get_base_dir();
			if (!common_base_dirs.has(base_dir)) {
				common_base_dirs.insert(base_dir, gdre::get_files_at(base_dir, {}));
			}
			String pack_base_name = path.get_file().get_basename();
			for (auto &p : common_base_dirs[base_dir]) {
				String file = p.get_file();
				String file_ext = file.get_extension().to_lower();
				if (file_ext == "pck" || file_ext == "zip" || file_ext == "ipa" || file_ext == "apk") {
					continue;
				}
				if (file.nocasecmp_to(pack_base_name) == 0 || file.get_basename().filenocasecmp_to(pack_base_name) == 0) {
					main_pck_path = path;
					pck_files.remove_at(i);
					break;
				}
			}
			if (!main_pck_path.is_empty()) {
				break;
			}
		}
	}

	if (!additional_main_pck_paths.is_empty()) {
		additional_main_pck_paths.append_array(pck_files);
		pck_files = additional_main_pck_paths;
	}

	if (!main_pck_path.is_empty()) {
		pck_files.insert(0, main_pck_path);
	}
	return pck_files;
}

Error GDRESettings::_add_pack(const String &path) {
	Error err = OK;
	auto san_path = sanitize_home_in_path(path);
	if (DirAccess::exists(path)) {
		print_line("Opening directory: " + san_path);
		err = load_dir(path);
		ERR_FAIL_COND_V_MSG(err == ERR_ALREADY_IN_USE, ERR_ALREADY_IN_USE, "Can't load project directory, already loaded from " + path);
		if (err || !is_pack_loaded()) {
			ERR_FAIL_COND_V_MSG(err, err, "Can't load project directory!!");
		}
		load_pack_uid_cache();
		load_pack_gdscript_cache();
		return OK;
	}

	print_line("Opening file: " + san_path);
	err = load_pck(path);
	ERR_FAIL_COND_V_MSG(err == ERR_ALREADY_IN_USE, ERR_ALREADY_IN_USE, "Can't load PCK, already loaded from " + path);
	if (err || !is_pack_loaded()) {
		ERR_FAIL_COND_V_MSG(err, err, "Can't load project!");
	}
	auto last_type = packs[packs.size() - 1]->type;
	if ((last_type == PackInfo::APK || last_type == PackInfo::ZIP) && has_path_loaded("res://assets.sparsepck")) {
		err = load_pck("res://assets.sparsepck");
		if (err && err != ERR_ALREADY_IN_USE) {
			if (error_encryption) {
				ERR_FAIL_COND_V_MSG(err, err, "Failed to load sparse pack! (Did you set the correct key?)");
			}
			ERR_FAIL_COND_V_MSG(err, err, "Failed to load sparse pack!!");
		}
		err = OK;
	}
	load_pack_uid_cache();
	load_pack_gdscript_cache();
	return OK;
}

Error GDRESettings::load_project(const Vector<String> &p_paths, bool _cmd_line_extract, const String &csharp_assembly_override) {
	GDRELogger::clear_error_queues();
	error_encryption = false;
	if (is_pack_loaded()) {
		return ERR_ALREADY_IN_USE;
	}

	if (p_paths.is_empty()) {
		ERR_FAIL_V_MSG(ERR_FILE_NOT_FOUND, "No valid paths provided!");
	}

	if (logger->get_path().is_empty()) {
		logger->start_prebuffering();
		log_sysinfo();
	}

	Error err = ERR_CANT_OPEN;
	Vector<String> pck_files = sort_and_validate_pck_files(p_paths);

	if (pck_files.is_empty()) {
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "No valid paths provided!");
	}

	for (int i = 0; i < pck_files.size(); i++) {
		err = _add_pack(pck_files[i]);
		if (err) {
			if (err == ERR_ALREADY_IN_USE) {
				continue;
			}
			bool crypto_error = had_encryption_error();
			unload_project(true);
			if (crypto_error) {
				ERR_FAIL_COND_V_MSG(err, ERR_UNAUTHORIZED, "Can't load project! (Did you set the correct key?)");
			}
			ERR_FAIL_COND_V_MSG(err, err, "Can't load project!");
		}
	}

	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), ERR_FILE_CANT_READ, "FATAL ERROR: loaded project pack, but didn't load files from it!");
	if (_cmd_line_extract) {
		return OK;
	}

	_init_bytecode_from_ephemeral_settings();

	return _project_post_load(true, csharp_assembly_override);
}

Error GDRESettings::_project_post_load(bool initial_load, const String &csharp_assembly_override) {
	Error err = OK;
	if (GDREConfig::get_singleton()->get_setting("load_embedded_zips", true)) {
		err = _load_embedded_zips();
		if (err == ERR_UNAVAILABLE) {
			err = OK;
		}
	}

	bool invalid_ver = !has_valid_version() || current_project->suspect_version;

	if (invalid_ver) {
		err = get_version_from_bin_resources();
		if (err) {
			unload_project(true);
			ERR_FAIL_V_MSG(err, "FATAL ERROR: Can't determine engine version of project pack!");
		}
		current_project->suspect_version = false;
	}

	err = detect_bytecode_revision(invalid_ver);
	if (err) {
		if (err == ERR_UNAUTHORIZED) {
			_set_error_encryption(true);
		}
		WARN_PRINT("Could not determine bytecode revision, not able to decompile scripts...");
	}

	ResourceCompatLoader::make_globally_available();

	if (!pack_has_project_config()) {
		WARN_PRINT("Could not find project configuration in directory, may be a seperate resource pack...");
	} else {
		if (is_project_config_loaded()) {
			print_line("Reloading project config...");
			current_project->pcfg = Ref<ProjectConfigLoader>(memnew(ProjectConfigLoader));
		}
		err = load_project_config();
		if (err != ERR_ALREADY_IN_USE) {
			ERR_FAIL_COND_V_MSG(err, err, "FATAL ERROR: Can't open project config!");
		}
		if (get_ver_major() <= 2) {
			auto remap_section = current_project->pcfg->get_section("remap");
			if (remap_section.has("all") || remap_section.is_empty()) {
				v2_remap_setting = "remap/all";
			} else {
				String setting = remap_section.begin()->key;
				v2_remap_setting = "remap/" + setting;
			}
		}
	}

	if (get_ver_major() == 0) {
		err = _load_obdb_resources();
		if (err) {
			ERR_FAIL_V_MSG(err, "FATAL ERROR: Could not load OBDB resources!");
		}
	}

	err = load_import_files();
	ERR_FAIL_COND_V_MSG(err, ERR_FILE_CANT_READ, "FATAL ERROR: Could not load imported binary files!");

	print_line(vformat("Loaded %d imported files", import_files.size()));

	_detect_csharp();
	if (project_requires_dotnet_assembly()) {
#if !GODOT_MONO_DECOMP_DISABLED
		if (!csharp_assembly_override.is_empty()) {
			err = reload_dotnet_assembly(csharp_assembly_override);
		} else if (!has_loaded_dotnet_assembly()) {
			err = load_project_dotnet_assembly();
		}
		if (err) {
			WARN_PRINT("Could not load C# assembly, not able to decompile C# scripts...");
		}
#else
		WARN_PRINT("C# assembly detected, but C# decompilation is disabled in this build of GDRE Tools.");
#endif
	}
	_ensure_script_cache_complete();

	_set_shader_globals();

	_get_app_version();
	print_line(vformat("Detected Engine Version: %s", get_version_string()));
	int bytecode_revision = get_bytecode_revision();
	if (bytecode_revision != 0) {
		auto decomp = GDScriptDecomp::create_decomp_for_commit(bytecode_revision);
		if (decomp.is_valid()) {
			print_line(vformat("Detected Bytecode Revision: %s (%07x)", decomp->get_engine_version(), bytecode_revision));
		}
	}

	return OK;
}

Error GDRESettings::_load_embedded_zips() {
	auto zip_files = get_file_list({ "*.zip" });
	bool has_zips_to_load = false;
	Error err = OK;
	if (zip_files.size() > 0) {
		Vector<String> pck_zip_files;
		for (auto path : get_pack_paths()) {
			if (path.has_extension("zip")) {
				pck_zip_files.push_back(path.get_file().to_lower());
			}
		}
		for (auto zip_file : zip_files) {
			if (is_zip_file_pack(zip_file) && !pck_zip_files.has(zip_file.get_file().to_lower())) {
				Error load_err = load_pck(zip_file);
				if (load_err == ERR_ALREADY_IN_USE) {
					continue;
				} else if (load_err) {
					err = load_err;
				}
				has_zips_to_load = true;
				ERR_CONTINUE_MSG(load_err, "Can't load embedded zip file: " + zip_file);
				load_pack_uid_cache();
				load_pack_gdscript_cache();
			}
		}
	}
	if (!has_zips_to_load) {
		return ERR_UNAVAILABLE;
	}
	return err;
}

Error GDRESettings::post_load_patch_translation() {
	if (!is_pack_loaded()) {
		return ERR_FILE_CANT_OPEN;
	}
	bool invalid_ver = !has_valid_version() || current_project->suspect_version;
	Error err = OK;
	if (invalid_ver) {
		err = get_version_from_bin_resources();
		if (err) {
			return err;
		}
		current_project->suspect_version = false;
	}
	if (!pack_has_project_config()) {
		WARN_PRINT("Could not find project configuration in directory, may be a seperate resource pack...");
	} else {
		err = load_project_config();
		ERR_FAIL_COND_V_MSG(err, err, "FATAL ERROR: Can't open project config!");
	}

	err = load_import_files();
	ERR_FAIL_COND_V_MSG(err, ERR_FILE_CANT_READ, "FATAL ERROR: Could not load imported binary files!");

	return err;
}

bool GDRESettings::needs_post_load_patch_translation() const {
	if (!is_pack_loaded()) {
		return false;
	}
	bool invalid_ver = !has_valid_version() || current_project->suspect_version;
	bool should_load_project_config = pack_has_project_config() && !is_project_config_loaded();
	return invalid_ver || should_load_project_config || import_files.is_empty();
}

constexpr bool GDRESettings::need_correct_patch(int ver_major, int ver_minor) {
	return ((ver_major == 2 || ver_major == 3) && ver_minor == 1);
}

Error GDRESettings::detect_bytecode_revision(bool p_no_valid_version) {
	if (!is_pack_loaded()) {
		return ERR_FILE_CANT_OPEN;
	}
	int ver_major = -1;
	int ver_minor = -1;
	if (has_valid_version()) {
		ver_major = get_ver_major();
		ver_minor = get_ver_minor();
	}
	Vector<String> bytecode_files = get_file_list({ "*.gdc" });
	Vector<String> encrypted_files = get_file_list({ "*.gde" });

	auto guess_from_version = [&](Error fail_error = ERR_FILE_CANT_OPEN) {
		if (ver_major > 0 && ver_minor >= 0) {
			auto decomp = GDScriptDecomp::create_decomp_for_version(current_project->version->as_text(), true);
			ERR_FAIL_COND_V_MSG(decomp.is_null(), fail_error, "Could not find bytecode revision for engine version: " + get_version_string());
			print_line("Guessing bytecode revision from engine version: " + get_version_string() + " (rev 0x" + String::num_int64(decomp->get_bytecode_rev(), 16) + ")");
			current_project->bytecode_revision = decomp->get_bytecode_rev();
			return OK;
		}
		current_project->bytecode_revision = 0;
		ERR_FAIL_V_MSG(fail_error, "Cannot determine bytecode revision!");
	};
	if (!encrypted_files.is_empty()) {
		auto file = encrypted_files[0];
		Vector<uint8_t> buffer;
		Error err = GDScriptDecomp::get_buffer_encrypted(file, ver_major > 0 ? ver_major : 3, enc_key, buffer);
		if (err) {
			current_project->bytecode_revision = 0;
		}
		ERR_FAIL_COND_V_MSG(err, ERR_UNAUTHORIZED, "Cannot determine bytecode revision: Encryption error (Did you set the correct key?)");
		bytecode_files.append_array(encrypted_files);
	}
	if (current_project->bytecode_revision != 0) {
		return OK;
	}

	if (bytecode_files.is_empty()) {
		return guess_from_version(ERR_PARSE_ERROR);
	}
	auto revision = BytecodeTester::test_files(bytecode_files, ver_major, ver_minor, true);
	if (revision == 0) {
		ERR_FAIL_COND_V_MSG(need_correct_patch(ver_major, ver_minor), ERR_FILE_CANT_OPEN, "Cannot determine bytecode revision: Need the correct patch version for engine version " + itos(ver_major) + "." + itos(ver_minor) + ".x!");
		return guess_from_version(ERR_FILE_CANT_OPEN);
	}
	current_project->bytecode_revision = revision;
	auto decomp = GDScriptDecomp::create_decomp_for_commit(revision);
	ERR_FAIL_COND_V_MSG(decomp.is_null(), ERR_FILE_CANT_OPEN, "Cannot determine bytecode revision!");
	auto check_if_same_minor_major = [&](Ref<GodotVer> version, Ref<GodotVer> max_ver) {
		if (!(max_ver->get_major() == version->get_major() && max_ver->get_minor() == version->get_minor())) {
			return false;
		}
		return true;
	};
	if (!has_valid_version()) {
		current_project->version = decomp->get_godot_ver();
		current_project->version->set_build_metadata("");
	} else {
		auto version = decomp->get_godot_ver();
		if (version->is_prerelease()) {
			current_project->version = decomp->get_max_engine_version().is_empty() ? version : decomp->get_max_godot_ver();
		} else if (ver_major < 3 || (ver_major == 3 && ver_minor <= 1) || p_no_valid_version) {
			auto max_version = decomp->get_max_godot_ver();
			if (max_version.is_valid() && (check_if_same_minor_major(current_project->version, max_version))) {
				if (max_version->get_patch() > current_project->version->get_patch()) {
					current_project->version->set_patch(max_version->get_patch());
				}
			} else if (check_if_same_minor_major(current_project->version, version)) {
				if (version->get_patch() > current_project->version->get_patch()) {
					current_project->version->set_patch(version->get_patch());
				}
			}
		}
	}
	return OK;
}

int GDRESettings::get_bytecode_revision() const {
	return is_pack_loaded() ? current_project->bytecode_revision : 0;
}

Error GDRESettings::get_version_from_bin_resources() {
	int consistent_versions = 0;
	int inconsistent_versions = 0;
	uint32_t ver_major = 0;
	uint32_t ver_minor = 0;
	int min_major = INT_MAX;
	int max_major = 0;
	int min_minor = INT_MAX;
	int max_minor = 0;

	int version_from_dir = get_ver_major_from_dir();

	Vector<String> bytecode_files = get_file_list({ "*.gdc" });
	Vector<Ref<GDScriptDecomp>> decomps;

	auto check_if_same_minor_major = [&](Ref<GodotVer> version, Ref<GodotVer> max_ver) {
		if (!(max_ver->get_major() == version->get_major() && max_ver->get_minor() == version->get_minor())) {
			return false;
		}
		return true;
	};
	auto set_min_max = [&](Ref<GodotVer> version) {
		min_minor = MIN(min_minor, version->get_minor());
		max_minor = MAX(max_minor, version->get_minor());
		min_major = MIN(min_major, version->get_major());
		max_major = MAX(max_major, version->get_major());
	};

	auto do_thing = [&]() {
		if (decomps.size() == 1) {
			auto version = decomps[0]->get_godot_ver();
			auto max_version = decomps[0]->get_max_godot_ver();
			if (version->get_major() != 4 && (max_version.is_null() || check_if_same_minor_major(version, max_version))) {
				current_project->version = max_version.is_valid() ? max_version : version;
				current_project->version->set_build_metadata("");
				return true;
			}
		};
		for (auto decomp : decomps) {
			auto version = decomp->get_godot_ver();
			auto max_version = decomp->get_max_godot_ver();
			set_min_max(version);
			if (max_version.is_valid()) {
				set_min_max(max_version);
			}
		}
		return false;
	};

	if (!bytecode_files.is_empty()) {
		decomps = BytecodeTester::get_possible_decomps(bytecode_files);
		if (decomps.is_empty()) {
			decomps = BytecodeTester::get_possible_decomps(bytecode_files, true);
		}
		if (decomps.is_empty()) {
			WARN_PRINT("Could not determine bytecode revision from bytecode files!");
		} else {
			if (do_thing()) {
				return OK;
			}
			if (min_major == max_major && min_minor == max_minor) {
				current_project->version = GodotVer::create(min_major, min_minor, 0);
				return OK;
			}
		}
	} else {
		min_minor = 0;
		max_minor = INT_MAX;
		min_major = 0;
		max_major = INT_MAX;
	}

	List<String> exts;
	ResourceCompatLoader::get_base_extensions(&exts, version_from_dir);
	Vector<String> wildcards;
	for (const String &ext : exts) {
		wildcards.push_back("*." + ext);
	}
	Vector<String> files = get_file_list(wildcards);
	int64_t max = files.size();
	bool sus_warning = false;

	for (int64_t i = 0; i < max; i++) {
		bool suspicious = false;
		uint32_t res_major = 0;
		uint32_t res_minor = 0;
		Error err = ResourceFormatLoaderCompatBinary::get_ver_major_minor(files[i], res_major, res_minor, suspicious);
		if (err) {
			continue;
		}
		if (!sus_warning && suspicious) {
			if (res_major == 3 && res_minor == 1) {
				WARN_PRINT("Warning: Found suspicious major/minor version, probably Sonic Colors Unlimited...");
				max = 1000;
			} else {
				WARN_PRINT("Warning: Found suspicious major/minor version...");
			}
			sus_warning = true;
		}
		if (consistent_versions == 0) {
			ver_major = res_major;
			ver_minor = res_minor;
		}
		if (ver_major == res_major && res_minor == ver_minor) {
			consistent_versions++;
		} else {
			if (ver_major != res_major) {
				WARN_PRINT_ONCE("WARNING!!!!! Inconsistent major versions in binary resources!");
				if (ver_major < res_major) {
					ver_major = res_major;
					ver_minor = res_minor;
				}
			} else if (ver_minor < res_minor) {
				ver_minor = res_minor;
			}
			inconsistent_versions++;
		}
	}
	if (inconsistent_versions > 0) {
		WARN_PRINT(itos(inconsistent_versions) + " binary resources had inconsistent versions!");
	}
	Vector<String> xml_files = get_file_list({ "*.xml" });
	if (ver_major == 0 && ver_minor == 0 && xml_files.is_empty()) {
		WARN_PRINT("Couldn't determine ver major from binary resources?!");
		ver_major = version_from_dir;
		ERR_FAIL_COND_V_MSG(ver_major == 0, ERR_CANT_ACQUIRE_RESOURCE, "Can't find version from directory!");
	}

	current_project->version = GodotVer::create(ver_major, ver_minor, 0);
	if (ver_major <= 2 && !xml_files.is_empty()) {
		Ref<RegEx> regex = RegEx::create_from_string("<resource_file.*version_name=\"Godot Engine v([^\"]+)\">");
		Ref<GodotVer> max_version = nullptr;
		for (auto xml_file : xml_files) {
			Ref<ResourceInfo> res_info;
			Ref<FileAccess> f = FileAccess::open(xml_file, FileAccess::READ);
			ERR_CONTINUE_MSG(f.is_null(), "Failed to open XML file: " + xml_file);
			String header = f->get_line();
			if (header.begins_with("<?xml version=")) {
				header = f->get_line();
			}
			if (!header.begins_with("<resource_file")) {
				continue;
			}
			auto match = regex->search(header);
			if (match.is_null()) {
				continue;
			}
			auto version_name = match->get_string(1);
			if (version_name.is_empty()) {
				continue;
			}
			auto version = GodotVer::parse(version_name);
			if (version.is_valid() && version->is_valid_semver() && (max_version.is_null() || version->gt(max_version))) {
				max_version = version;
			}
		}
		if (max_version.is_valid() && max_version->gte(current_project->version)) {
			current_project->version = max_version;
		}
	}
	return OK;
}

Error GDRESettings::load_project_config() {
	Error err;
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), ERR_FILE_CANT_OPEN, "Pack not loaded!");
	ERR_FAIL_COND_V_MSG(is_project_config_loaded(), ERR_ALREADY_IN_USE, "Project config is already loaded!");
	ERR_FAIL_COND_V_MSG(!pack_has_project_config(), ERR_FILE_NOT_FOUND, "Could not find project config!");
	if (get_ver_major() <= 2) {
		err = current_project->pcfg->load_cfb(has_path_loaded("res://engine.cfb") ? "res://engine.cfb" : "res://engine.cfg", get_ver_major(), get_ver_minor());
		ERR_FAIL_COND_V_MSG(err, err, "Failed to load project config!");
	} else if (get_ver_major() >= 3) {
		err = current_project->pcfg->load_cfb(has_path_loaded("res://project.binary") ? "res://project.binary" : "res://project.godot", get_ver_major(), get_ver_minor());
		ERR_FAIL_COND_V_MSG(err, err, "Failed to load project config!");
	} else {
		ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED,
				"Godot version not set or project uses unsupported Godot version");
	}
	return OK;
}

Error GDRESettings::save_project_config(const String &p_out_dir = "") {
	String output_dir = p_out_dir;
	if (output_dir.is_empty()) {
		output_dir = project_path;
	}
	return current_project->pcfg->save_cfb(output_dir, get_ver_major(), get_ver_minor());
}

Error GDRESettings::save_project_config_binary(const String &p_out_dir = "") {
	String output_dir = p_out_dir;
	if (output_dir.is_empty()) {
		output_dir = project_path;
	}
	return current_project->pcfg->save_cfb_binary(output_dir, get_ver_major(), get_ver_minor());
}

Error GDRESettings::unload_project(bool p_no_reset_ephemeral) {
	logger->stop_prebuffering();
	GDREPackedData::get_singleton()->clear();
	if (custom_decryptor.is_valid() && custom_decryption_script_path.is_empty()) {
		custom_decryptor = nullptr;
	}
	v2_remap_setting = "remap/all";
	ResourceCompatLoader::unmake_globally_available();
	if (!is_pack_loaded()) {
		return ERR_DOES_NOT_EXIST;
	}
	_clear_shader_globals();
	error_encryption = false;

	remove_current_pack();
	reset_uid_cache();
	reset_gdscript_cache();
	if (!p_no_reset_ephemeral && GDREConfig::get_singleton()) {
		GDREConfig::get_singleton()->reset_ephemeral_settings();
	}
	return OK;
}

void GDRESettings::add_pack_info(Ref<PackInfo> packinfo) {
	ERR_FAIL_COND_MSG(!packinfo.is_valid(), "Invalid pack info!");
	packs.push_back(packinfo);
	if (!current_project.is_valid()) {
		current_project = Ref<ProjectInfo>(memnew(ProjectInfo));
		current_project->version = version_override.is_valid() ? version_override : GodotVer::copy_from(packinfo->version);
		current_project->pack_file = packinfo->pack_file;
		current_project->type = packinfo->type;
		current_project->suspect_version = packinfo->suspect_version;
		current_project->non_standard_header = packinfo->non_standard_header;
		current_project->app_version = packinfo->app_version;
	} else {
		if (current_project->app_version.is_empty() && !packinfo->app_version.is_empty()) {
			current_project->app_version = packinfo->app_version;
		}
		if (!version_override.is_valid() && !current_project->version->eq(packinfo->version)) {
			if ((!current_project->version->is_valid_semver() || current_project->version->get_major() == 0) &&
					packinfo->version->is_valid_semver() && packinfo->version->get_major() != 0) {
				current_project->version = GodotVer::copy_from(packinfo->version);
			} else {
				WARN_PRINT("Warning: Pack version mismatch!");
			}
		}
	}
}

Error GDRESettings::add_custom_pack_source_script(const String &p_script_path) {
	if (is_pack_loaded()) {
		ERR_FAIL_V_MSG(ERR_ALREADY_IN_USE, "Cannot set custom pack source script after pack is loaded!");
	}
	if (p_script_path.is_empty()) {
		GDREPackedData::get_singleton()->clear_custom_pack_sources();
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!FileAccess::exists(p_script_path), ERR_FILE_NOT_FOUND, "Custom pack source script file '" + p_script_path + "' does not exist");
	ERR_FAIL_COND_V_MSG(p_script_path.get_extension().to_lower() != "gd", ERR_INVALID_PARAMETER, "Custom pack source script file must be a GDScript!");
	Error err = OK;
	ResourceFormatLoaderGDScript loader;
	Ref<Script> script = loader.load(p_script_path, p_script_path, &err, false, nullptr, ResourceFormatLoader::CACHE_MODE_IGNORE);
	ERR_FAIL_COND_V_MSG(script.is_null() || err != OK, err, "Failed to load custom pack source script!");
	auto base_type = script->get_instance_base_type();
	ERR_FAIL_COND_V_MSG(base_type != "PackSourceCustom", ERR_INVALID_PARAMETER, "Custom pack source script does not inherit from PackSourceCustom!");
	Ref<PackSourceCustom> pack_source;
	pack_source.instantiate();
	pack_source->set_script(script);
	ERR_FAIL_COND_V_MSG(Ref<Script>(pack_source->get_script()).is_null(), ERR_INVALID_PARAMETER, "Failed to instantiate custom pack source script!");
	ERR_FAIL_NULL_V_MSG(pack_source->get_script_instance(), ERR_INVALID_PARAMETER, "Failed to get script instance from custom pack source script!");
	GDREPackedData::get_singleton()->clear_custom_pack_sources();
	GDREPackedData::get_singleton()->add_custom_pack_source(pack_source);
	return OK;
}

void GDRESettings::clear_custom_pack_source_script() {
	if (is_pack_loaded()) {
		ERR_FAIL_MSG("Cannot clear custom pack source script after pack is loaded!");
	}

	GDREPackedData::get_singleton()->clear_custom_pack_sources();
}

StringName GDRESettings::get_cached_script_class(const String &p_path) {
	if (!is_pack_loaded() || p_path.is_empty()) {
		return "";
	}
	String path = p_path;
	if (!script_cache.has(path)) {
		path = get_mapped_path(p_path);
	}
	if (script_cache.has(path)) {
		auto &dict = script_cache.get(path);
		if (dict.has("class")) {
			return dict["class"];
		}
	}
	return "";
}

StringName GDRESettings::get_cached_script_base(const String &p_path) {
	if (!is_pack_loaded() || p_path.is_empty()) {
		return "";
	}
	String path = p_path;
	if (!script_cache.has(path)) {
		path = get_mapped_path(p_path);
	}
	if (script_cache.has(p_path)) {
		auto &dict = script_cache[p_path];
		if (dict.has("base")) {
			return dict["base"];
		}
	}
	return "";
}

String GDRESettings::get_path_for_script_class(const StringName &p_class) {
	if (!is_pack_loaded() || p_class.is_empty()) {
		return "";
	}
	for (auto kv : script_cache) {
		auto &dict = kv.value;
		if (dict.get("class", "").operator StringName() == p_class) {
			return kv.key;
		}
	}
	return "";
}

Dictionary GDRESettings::get_cached_script_entry(const String &p_path) {
	if (!is_pack_loaded() || p_path.is_empty() || !script_cache.has(p_path)) {
		return Dictionary();
	}
	return script_cache[p_path];
}

bool GDRESettings::had_encryption_error() const {
	return error_encryption;
}
void GDRESettings::_set_error_encryption(bool is_encryption_error) {
	error_encryption = is_encryption_error;
}

Vector<uint8_t> GDRESettings::get_encryption_key() {
	return enc_key;
}

String GDRESettings::get_encryption_key_string() {
	if (enc_key.is_empty()) {
		return "";
	}
	return String::hex_encode_buffer(enc_key.ptr(), enc_key.size());
}

int GDRESettings::get_required_key_size_in_bytes() const {
	int result = 32;
	if (custom_decryptor.is_valid()) {
		result = custom_decryptor->get_required_key_size_in_bytes();
	}
	return result;
}

Error GDRESettings::set_encryption_key(Vector<uint8_t> key) {
	if (key.size() != get_required_key_size_in_bytes()) {
		return ERR_INVALID_PARAMETER;
	}
	enc_key = key;
	return OK;
}

Error GDRESettings::set_encryption_key_string(const String &key_str) {
	String skey = key_str.replace_first("0x", "");
	ERR_FAIL_COND_V_MSG(!skey.is_valid_hex_number(false) || skey.size() < 64, ERR_INVALID_PARAMETER, "not a valid key");

	Vector<uint8_t> key;
	int key_size = get_required_key_size_in_bytes();
	key.resize(key_size);
	for (int i = 0; i < key_size; i++) {
		int v = 0;
		if (i * 2 < skey.length()) {
			char32_t ct = skey.to_lower()[i * 2];
			if (ct >= '0' && ct <= '9') {
				ct = ct - '0';
			} else if (ct >= 'a' && ct <= 'f') {
				ct = 10 + ct - 'a';
			}
			v |= ct << 4;
		}

		if (i * 2 + 1 < skey.length()) {
			char32_t ct = skey.to_lower()[i * 2 + 1];
			if (ct >= '0' && ct <= '9') {
				ct = ct - '0';
			} else if (ct >= 'a' && ct <= 'f') {
				ct = 10 + ct - 'a';
			}
			v |= ct;
		}
		key.write[i] = v;
	}
	set_encryption_key(key);
	return OK;
}

Error GDRESettings::set_custom_decryption_script(const String &p_decryptor_script_path) {
	ERR_FAIL_COND_V_MSG(!FileAccess::exists(p_decryptor_script_path), ERR_FILE_NOT_FOUND, "Custom encryption script file '" + p_decryptor_script_path + "' does not exist");
	ERR_FAIL_COND_V_MSG(p_decryptor_script_path.get_extension().to_lower() != "gd", ERR_INVALID_PARAMETER, "Custom encryption script file must be a GDScript!");
	Error err = OK;
	ResourceFormatLoaderGDScript loader;
	Ref<Script> script = loader.load(p_decryptor_script_path, p_decryptor_script_path, &err, false, nullptr, ResourceFormatLoader::CACHE_MODE_IGNORE);
	ERR_FAIL_COND_V_MSG(script.is_null() || err != OK, err, "Failed to load custom encryption script!");
	auto base_type = script->get_instance_base_type();
	ERR_FAIL_COND_V_MSG(base_type != "CustomDecryptor", ERR_INVALID_PARAMETER, "Custom encryption script does not inherit from CustomDecryptor!");
	Ref<CustomDecryptor> decryptor;
	decryptor.instantiate();
	decryptor->set_script(script);
	ERR_FAIL_COND_V_MSG(Ref<Script>(decryptor->get_script()).is_null(), ERR_INVALID_PARAMETER, "Failed to instantiate custom encryption script!");
	ERR_FAIL_NULL_V_MSG(decryptor->get_script_instance(), ERR_INVALID_PARAMETER, "Failed to get script instance from custom encryption script!");
	custom_decryption_script_path = p_decryptor_script_path;
	set_custom_decryptor(decryptor);
	return OK;
}

void GDRESettings::set_custom_decryptor(const Ref<CustomDecryptor> &p_decryptor) {
	custom_decryptor = p_decryptor;
}

Ref<CustomDecryptor> GDRESettings::get_custom_decryptor() const {
	return custom_decryptor;
}

String GDRESettings::get_custom_decryption_script_path() const {
	return custom_decryption_script_path;
}

void GDRESettings::reset_custom_decryptor() {
	custom_decryptor = nullptr;
	custom_decryption_script_path = "";
}

void GDRESettings::reset_encryption_key() {
	enc_key.clear();
}

Vector<String> GDRESettings::get_file_list(const Vector<String> &filters) {
	if (!is_pack_loaded()) {
		return gdre::get_recursive_dir_list("res://", filters);
	}
	Vector<String> ret;
	Vector<Ref<PackedFileInfo>> flist = get_file_info_list(filters);
	for (int i = 0; i < flist.size(); i++) {
		ret.push_back(flist[i]->path);
	}
	return ret;
}

Array GDRESettings::get_file_info_array(const Vector<String> &filters) {
	Array ret;
	for (auto file_info : get_file_info_list(filters)) {
		ret.push_back(file_info);
	}
	return ret;
}

Vector<Ref<PackedFileInfo>> GDRESettings::get_file_info_list(const Vector<String> &filters) {
	return GDREPackedData::get_singleton()->get_file_info_list(filters);
}

TypedArray<PackInfo> GDRESettings::get_pack_info_list() const {
	TypedArray<PackInfo> ret;
	for (const auto &pack : packs) {
		ret.push_back(pack);
	}
	return ret;
}

Vector<String> GDRESettings::get_pack_paths() const {
	Vector<String> ret;
	for (const auto &pack : packs) {
		ret.push_back(pack->pack_file);
	}
	return ret;
}

String GDRESettings::localize_path(const String &p_path, const String &resource_dir) const {
	String res_path = resource_dir != "" ? resource_dir : project_path;
#ifdef TOOLS_ENABLED
	if (res_path.is_empty() && Engine::get_singleton()->is_editor_hint()) {
		res_path = ProjectSettings::get_singleton()->get_resource_path();
	}
#endif

	if (p_path.begins_with("res://") || p_path.begins_with("user://")) {
		return p_path.simplify_path();
	}
	if (!res_path.is_empty() && p_path.simplify_path().begins_with(res_path)) {
		return p_path.replace(res_path, "res://").simplify_path();
	}
	if ((p_path.is_absolute_path())) {
		if (!res_path.is_empty()) {
			if (p_path.begins_with(res_path)) {
				return p_path.replace(res_path, "res://").simplify_path();
			}
			String path = p_path.simplify_path();
			if (path.begins_with(res_path)) {
				return path.replace(res_path, "res://").simplify_path();
			}
		}
		if (is_pack_loaded() && (res_path == "" || !p_path.begins_with(res_path))) {
			String dir_path = p_path.get_base_dir().simplify_path();
			while (!dir_path.is_empty() && !DirAccess::dir_exists_absolute("res://" + dir_path)) {
				auto parts = dir_path.split("/", false, 1);
				if (parts.size() < 2) {
					dir_path = "";
					break;
				}
				dir_path = parts[1];
			}
			if (!dir_path.is_empty()) {
				return "res://" + dir_path.path_join(p_path.get_file());
			}
		}
		return p_path.simplify_path();
	}

	if (res_path == "") {
		if (!p_path.is_absolute_path()) {
			return "res://" + p_path;
		}
		return p_path.simplify_path();
	}

	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);

	String path = p_path.replace("\\", "/").simplify_path();

	if (dir->change_dir(path) == OK) {
		String cwd = dir->get_current_dir();
		cwd = cwd.replace("\\", "/");

		res_path = res_path.path_join("");
		cwd = cwd.path_join("");

		if (!cwd.begins_with(res_path)) {
			return p_path;
		}

		return cwd.replace_first(res_path, "res://");
	} else {
		int sep = path.rfind("/");
		if (sep == -1) {
			return "res://" + path;
		}

		String parent = path.substr(0, sep);

		String plocal = localize_path(parent, res_path);
		if (plocal == "") {
			return "";
		}
		if (plocal[plocal.length() - 1] == '/') {
			sep += 1;
		}
		return plocal + path.substr(sep, path.size() - sep);
	}
}

String GDRESettings::globalize_path(const String &p_path, const String &resource_dir) const {
	String res_path = resource_dir != "" ? resource_dir : project_path;
#ifdef TOOLS_ENABLED
	if (res_path.is_empty() && Engine::get_singleton()->is_editor_hint()) {
		res_path = ProjectSettings::get_singleton()->get_resource_path();
	}
#endif

	if (p_path.begins_with("res://")) {
		if (res_path != "") {
			return p_path.replace("res:/", res_path);
		}
		return p_path.replace("res://", "");
	} else if (p_path.begins_with("user://")) {
		String data_dir = OS::get_singleton()->get_user_data_dir();
		if (data_dir != "") {
			return p_path.replace("user:/", data_dir);
		}
		return p_path.replace("user://", "");
	} else if (!p_path.is_absolute_path()) {
		return res_path.path_join(p_path);
	}

	return p_path;
}

bool GDRESettings::has_any_remaps() const {
	if (is_pack_loaded()) {
		if (get_ver_major() >= 3) {
			if (remap_iinfo.size() > 0) {
				return true;
			}
			if (current_project->pcfg->is_loaded() && current_project->pcfg->has_setting("path_remap/remapped_paths")) {
				return true;
			}
		} else {
			if (current_project->pcfg->is_loaded() && current_project->pcfg->has_setting(v2_remap_setting)) {
				return true;
			}
		}
	}
	return false;
}

Dictionary GDRESettings::get_remaps(bool include_imports) const {
	Dictionary ret;
	if (is_pack_loaded()) {
		if (get_ver_major() >= 3) {
			for (auto E : remap_iinfo) {
				ret[E.key] = E.value->get_path();
			}
			if (current_project->pcfg->is_loaded() && current_project->pcfg->has_setting("path_remap/remapped_paths")) {
				PackedStringArray v3remaps = current_project->pcfg->get_setting("path_remap/remapped_paths", PackedStringArray());
				for (int i = 0; i < v3remaps.size(); i += 2) {
					ret[v3remaps[i]] = v3remaps[i + 1];
				}
			}
		} else {
			if (current_project->pcfg->is_loaded() && current_project->pcfg->has_setting(v2_remap_setting)) {
				PackedStringArray v2remaps = current_project->pcfg->get_setting(v2_remap_setting, PackedStringArray());
				for (int i = 0; i < v2remaps.size(); i += 2) {
					ret[v2remaps[i]] = v2remaps[i + 1];
				}
			}
		}
		if (include_imports) {
			for (auto &[path, iinfo] : import_files) {
				ret[iinfo->get_source_file()] = iinfo->get_path();
			}
		}
	}
	return ret;
}

namespace {
bool has_old_remap(const Vector<String> &remaps, const String &src, const String &dst) {
	int idx = remaps.find(src);
	if (idx != -1 && idx % 2 == 0) {
		if (dst.is_empty()) {
			return true;
		}
		return idx + 1 >= remaps.size() ? false : remaps[idx + 1] == dst;
	}
	return false;
}

String get_mapped_path_unloaded(const String &p_path) {
	String src = p_path;
	if (src.begins_with("uid://")) {
		auto id = ResourceUID::get_singleton()->text_to_id(src);
		if (ResourceUID::get_singleton()->has_id(id)) {
			src = ResourceUID::get_singleton()->get_id_path(id);
		} else {
			return "";
		}
	}

	String iinfo_path = src + ".remap";
	if (FileAccess::exists(iinfo_path)) {
		String new_path = ImportInfoRemap::get_remap_path_from_file(iinfo_path);
		if (!new_path.is_empty()) {
			return new_path;
		}
	}
	iinfo_path = src + ".import";
	if (FileAccess::exists(iinfo_path)) {
		String new_path = ImportInfoModern::get_remap_path_from_file(iinfo_path);
		if (!new_path.is_empty()) {
			return new_path;
		}
	}
	return src;
}
} //namespace

String GDRESettings::get_remapped_source_path(const String &p_dst) const {
	if (is_pack_loaded()) {
		if (get_ver_major() >= 3) {
			for (auto E : remap_iinfo) {
				if (E.value->get_path() == p_dst) {
					return E.key;
				}
			}
		}
		String setting = get_ver_major() < 3 ? v2_remap_setting : "path_remap/remapped_paths";
		if (is_project_config_loaded() && current_project->pcfg->has_setting(setting)) {
			PackedStringArray remaps = current_project->pcfg->get_setting(setting, PackedStringArray());
			int idx = remaps.find(p_dst);
			if (idx != -1 && idx % 2 == 1 && idx - 1 >= 0) {
				return remaps[idx - 1];
			}
		}
	}
	return "";
}

String GDRESettings::get_mapped_path(const String &p_src) const {
	if (!is_pack_loaded()) {
		return get_mapped_path_unloaded(p_src);
	}
	String src = p_src;
	if (src.begins_with("uid://")) {
		auto id = ResourceUID::get_singleton()->text_to_id(src);
		if (ResourceUID::get_singleton()->has_id(id)) {
			src = ResourceUID::get_singleton()->get_id_path(id);
		} else {
			return "";
		}
	}
	String local_src = localize_path(src);
	String remapped_path = get_remap(local_src);
	if (!remapped_path.is_empty()) {
		return remapped_path;
	}
	String import_path = local_src + ".import";
	if (import_files.has(import_path)) {
		return import_files[import_path]->get_path();
	}

	for (auto &[path, iinfo] : import_files) {
		if (iinfo->get_source_file().nocasecmp_to(local_src) == 0) {
			return iinfo->get_path();
		}
	}
	return src;
}

String GDRESettings::get_remap(const String &src) const {
	if (is_pack_loaded()) {
		String local_src = localize_path(src);
		if (get_ver_major() >= 3) {
			String remap_file = local_src + ".remap";
			if (remap_iinfo.has(remap_file)) {
				return remap_iinfo[remap_file]->get_path();
			}
		}
		String setting = get_ver_major() < 3 ? v2_remap_setting : "path_remap/remapped_paths";
		if (is_project_config_loaded() && current_project->pcfg->has_setting(setting)) {
			PackedStringArray remaps = current_project->pcfg->get_setting(setting, PackedStringArray());
			int idx = remaps.find(local_src);
			if (idx != -1 && idx + 1 < remaps.size() && idx % 2 == 0) {
				return remaps[idx + 1];
			}
		}
	}
	return "";
}

bool GDRESettings::has_remap(const String &src, const String &dst) const {
	if (is_pack_loaded()) {
		String local_src = localize_path(src);
		String local_dst = !dst.is_empty() ? localize_path(dst) : "";
		if (get_ver_major() >= 3) {
			String remap_file = local_src + ".remap";
			if (remap_iinfo.has(remap_file)) {
				if (dst.is_empty()) {
					return true;
				}
				String dest_file = remap_iinfo[remap_file]->get_path();
				return dest_file == local_dst;
			}
		}
		String setting = get_ver_major() < 3 ? v2_remap_setting : "path_remap/remapped_paths";
		if (is_project_config_loaded() && current_project->pcfg->has_setting(setting)) {
			return has_old_remap(current_project->pcfg->get_setting(setting, PackedStringArray()), local_src, local_dst);
		}
	}
	return false;
}

Error GDRESettings::add_remap(const String &src, const String &dst) {
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), ERR_DATABASE_CANT_READ, "Pack not loaded!");
	if (get_ver_major() >= 3) {
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "Adding Remaps is not supported in 3.x-4.x packs yet!");
	}
	ERR_FAIL_COND_V_MSG(!is_project_config_loaded(), ERR_DATABASE_CANT_READ, "project config not loaded!");
	String setting = get_ver_major() < 3 ? v2_remap_setting : "path_remap/remapped_paths";
	PackedStringArray v2remaps = current_project->pcfg->get_setting(setting, PackedStringArray());
	String local_src = localize_path(src);
	String local_dst = localize_path(dst);
	int idx = v2remaps.find(local_src);
	if (idx != -1) {
		v2remaps.write[idx + 1] = local_dst;
	} else {
		v2remaps.push_back(local_src);
		v2remaps.push_back(local_dst);
	}
	current_project->pcfg->set_setting(setting, v2remaps);
	return OK;
}

Error GDRESettings::remove_remap(const String &src, const String &dst, const String &output_dir) {
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), ERR_DATABASE_CANT_READ, "Pack not loaded!");
	Error err;
	if (get_ver_major() >= 3) {
		ERR_FAIL_COND_V_MSG(output_dir.is_empty(), ERR_INVALID_PARAMETER, "Output directory must be specified for 3.x-4.x packs!");
		String remap_file = localize_path(src) + ".remap";
		if (remap_iinfo.has(remap_file)) {
			if (!dst.is_empty()) {
				String dest_file = remap_iinfo[remap_file]->get_path();
				if (dest_file != localize_path(dst)) {
					ERR_FAIL_V_MSG(ERR_DOES_NOT_EXIST, "Remap between" + src + " and " + dst + " does not exist!");
				}
			}
			remap_iinfo.erase(remap_file);
			Ref<DirAccess> da = DirAccess::open(output_dir, &err);
			ERR_FAIL_COND_V_MSG(err, err, "Can't open directory " + output_dir);
			String dest_path = output_dir.path_join(remap_file.replace("res://", ""));
			if (!FileAccess::exists(dest_path)) {
				return ERR_FILE_NOT_FOUND;
			}
			return da->remove(dest_path);
		}
	}
	if (!is_project_config_loaded()) {
		ERR_FAIL_COND_V_MSG(get_ver_major() < 3, ERR_DATABASE_CANT_READ, "project config not loaded!");
		ERR_FAIL_V_MSG(ERR_DOES_NOT_EXIST, "Remap between" + src + " and " + dst + " does not exist!");
	}
	String setting = get_ver_major() < 3 ? v2_remap_setting : "path_remap/remapped_paths";
	ERR_FAIL_COND_V_MSG(!current_project->pcfg->has_setting(setting), ERR_DOES_NOT_EXIST, "Remap between" + src + " and " + dst + " does not exist!");
	PackedStringArray v2remaps = current_project->pcfg->get_setting(setting, PackedStringArray());
	String local_src = localize_path(src);
	String local_dst = localize_path(dst);
	if (has_old_remap(v2remaps, local_src, local_dst)) {
		v2remaps.erase(local_src);
		v2remaps.erase(local_dst);
		if (v2remaps.size()) {
			err = current_project->pcfg->set_setting(v2_remap_setting, v2remaps);
		} else {
			err = current_project->pcfg->remove_setting(v2_remap_setting);
		}
		return err;
	}
	ERR_FAIL_V_MSG(ERR_DOES_NOT_EXIST, "Remap between" + src + " and " + dst + " does not exist!");
}

bool GDRESettings::has_project_setting(const String &p_setting) {
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), false, "Pack not loaded!");
	if (!is_project_config_loaded()) {
		WARN_PRINT("Attempted to check project setting " + p_setting + ", but no project config loaded");
		return false;
	}
	return current_project->pcfg->has_setting(p_setting);
}

Variant GDRESettings::get_project_setting(const String &p_setting, const Variant &default_value) const {
	ERR_FAIL_COND_V_MSG(!is_pack_loaded(), default_value, "Pack not loaded!");
	ERR_FAIL_COND_V_MSG(!is_project_config_loaded(), default_value, "project config not loaded!");
	return current_project->pcfg->get_setting(p_setting, default_value);
}

void GDRESettings::set_project_setting(const String &p_setting, Variant value) {
	ERR_FAIL_COND_MSG(!is_pack_loaded(), "Pack not loaded!");
	ERR_FAIL_COND_MSG(!is_project_config_loaded(), "project config not loaded!");
	current_project->pcfg->set_setting(p_setting, value);
}

String GDRESettings::get_project_config_path() {
	ERR_FAIL_COND_V_MSG(!is_project_config_loaded(), String(), "project config not loaded!");
	return current_project->pcfg->get_cfg_path();
}

String GDRESettings::get_log_file_path() {
	if (!logger) {
		return "";
	}
	return logger->get_path();
}

bool GDRESettings::is_headless() const {
	return headless;
}

float GDRESettings::get_auto_display_scale() {
	if (!DisplayServer::get_singleton()) {
		WARN_PRINT("DisplayServer not available, returning 1.0");
		return 1.0;
	}
#ifdef LINUXBSD_ENABLED
	if (DisplayServer::get_singleton()->get_name() == "Wayland") {
		float main_window_scale = DisplayServer::get_singleton()->screen_get_scale(DisplayServerEnums::SCREEN_OF_MAIN_WINDOW);

		if (DisplayServer::get_singleton()->get_screen_count() == 1 || Math::fract(main_window_scale) != 0) {
			return main_window_scale;
		}

		return DisplayServer::get_singleton()->screen_get_max_scale();
	}
#endif

#if defined(MACOS_ENABLED) || defined(ANDROID_ENABLED)
	return DisplayServer::get_singleton()->screen_get_max_scale();
#else
	const int screen = DisplayServer::get_singleton()->window_get_current_screen();

	if (DisplayServer::get_singleton()->screen_get_size(screen) == Vector2i()) {
		return 1.0;
	}

	const int smallest_dimension = MIN(DisplayServer::get_singleton()->screen_get_size(screen).x, DisplayServer::get_singleton()->screen_get_size(screen).y);
	if (DisplayServer::get_singleton()->screen_get_dpi(screen) >= 192 && smallest_dimension >= 1400) {
		return 2.0;
	} else if (smallest_dimension >= 1700) {
		return 1.5;
	} else if (smallest_dimension <= 800) {
		return 0.75;
	}
	return 1.0;
#endif
}

String GDRESettings::get_sys_info_string() const {
	String OS_Name = OS::get_singleton()->get_distribution_name();
	String OS_Version = OS::get_singleton()->get_version();
	String adapter_name = RenderingServer::get_singleton() ? RenderingServer::get_singleton()->get_video_adapter_name() : "";
	String render_driver = OS::get_singleton()->get_current_rendering_driver_name();
	if (adapter_name.is_empty()) {
		adapter_name = "headless";
	} else {
		adapter_name += ", " + render_driver;
	}

	return OS_Name + " " + OS_Version + ", " + adapter_name;
}

void GDRESettings::log_sysinfo() {
	print_line("GDRE Tools " + String(GDRE_VERSION));
	print_line(get_sys_info_string());
}

Error GDRESettings::open_log_file(const String &output_dir) {
	String logfile = output_dir.path_join("gdre_export.log");
	bool was_buffering = logger->is_prebuffering_enabled();
	Error err = logger->open_file(logfile);
	if (!was_buffering) {
		log_sysinfo();
	}
	ERR_FAIL_COND_V_MSG(err == ERR_ALREADY_IN_USE, err, "Already logging to another file");
	ERR_FAIL_COND_V_MSG(err != OK, err, "Could not open log file " + logfile);
	return OK;
}

Error GDRESettings::close_log_file() {
	logger->close_file();
	return OK;
}

Array GDRESettings::get_import_files(bool copy) {
	Array ifiles;
	for (auto &[path, iinfo] : import_files) {
		ifiles.push_back(copy ? ImportInfo::copy(iinfo) : iinfo);
	}
	return ifiles;
}

bool GDRESettings::has_path_loaded(const String &p_path) const {
	if (is_pack_loaded()) {
		return GDREPackedData::get_singleton()->has_path(p_path);
	}
	return false;
}

String GDRESettings::get_loaded_pack_data_dir() {
	String data_dir = "res://.godot";
	if (is_project_config_loaded()) {
		return current_project->pcfg->get_setting(
					   "application/config/use_hidden_project_data_directory",
					   true)
				? data_dir
				: "res://godot";
	}
	if (!DirAccess::exists(data_dir) && DirAccess::exists("res://godot")) {
		return "res://godot";
	}

	return data_dir;
}

Error GDRESettings::load_pack_uid_cache(bool p_reset) {
	if (!is_pack_loaded()) {
		return ERR_UNAVAILABLE;
	}
	String cache_file = get_loaded_pack_data_dir().path_join("uid_cache.bin");
	if (!FileAccess::exists(cache_file)) {
		return ERR_FILE_NOT_FOUND;
	}
	Ref<FileAccess> f = FileAccess::open(cache_file, FileAccess::READ);

	if (f.is_null()) {
		return ERR_CANT_OPEN;
	}

	if (p_reset) {
		ResourceUID::get_singleton()->clear();
		unique_ids.clear();
		path_to_uid.clear();
	}

	uint32_t entry_count = f->get_32();
	for (uint32_t i = 0; i < entry_count; i++) {
		int64_t id = f->get_64();
		int32_t len = f->get_32();
		UID_Cache c;
		c.cs.resize_uninitialized(len + 1);
		ERR_FAIL_COND_V(c.cs.size() != len + 1, ERR_FILE_CORRUPT);
		c.cs[len] = 0;
		int32_t rl = f->get_buffer((uint8_t *)c.cs.ptrw(), len);
		ERR_FAIL_COND_V(rl != len, ERR_FILE_CORRUPT);

		c.saved_to_cache = true;
		unique_ids[id] = c;
		path_to_uid[String::utf8(c.cs)] = id;
	}
	Vector<String> dupes;
	for (auto E : path_to_uid) {
		if (ResourceUID::get_singleton()->has_id(E.second)) {
			String old_path = ResourceUID::get_singleton()->get_id_path(E.second);
			String new_path = E.first;
			if (old_path != new_path) {
				if (old_path.simplify_path() == new_path.simplify_path()) {
					new_path = new_path.simplify_path();
				} else if (has_path_loaded(get_mapped_path_unloaded(old_path))) {
					if (!has_path_loaded(get_mapped_path_unloaded(new_path))) {
						continue;
					}
					dupes.push_back(ResourceUID::get_singleton()->id_to_text(E.second) + " -> " + old_path + "\n    Replacing with: " + new_path);
				} else if (!has_path_loaded(get_mapped_path_unloaded(new_path))) {
					dupes.push_back(ResourceUID::get_singleton()->id_to_text(E.second) + " -> " + old_path + "\n    Replacing with: " + new_path);
				}
			}

			ResourceUID::get_singleton()->set_id(E.second, new_path);
		} else {
			ResourceUID::get_singleton()->add_id(E.second, E.first);
		}
	}
	if (dupes.size() > 0) {
		WARN_PRINT("Duplicate IDs found in cache:\n  " + String("\n  ").join(dupes));
	}
#ifdef TOOLS_ENABLED
	if (!EditorNode::get_singleton()) {
		ResourceSaver::set_get_resource_id_for_path(&GDRESettings::_get_uid_for_path);
	}
#else
	ResourceSaver::set_get_resource_id_for_path(&GDRESettings::_get_uid_for_path);
#endif
	return OK;
}

Error GDRESettings::reset_uid_cache() {
	unique_ids.clear();
	path_to_uid.clear();
	ResourceUID::get_singleton()->clear();
	return ResourceUID::get_singleton()->load_from_cache(true);
}

ResourceUID::ID GDRESettings::_get_uid_for_path(const String &p_path, bool _generate) {
	return get_singleton()->get_uid_for_path(p_path);
}

ResourceUID::ID GDRESettings::get_uid_for_path(const String &p_path) const {
	ResourceUID::ID id = ResourceUID::INVALID_ID;
	path_to_uid.if_contains(p_path, [&](const ParallelFlatHashMap<String, ResourceUID::ID>::value_type &e) {
		id = e.second;
	});
#ifdef TOOLS_ENABLED
	if (id == ResourceUID::INVALID_ID && Engine::get_singleton()->is_editor_hint()) {
		id = ResourceUID::get_singleton()->get_path_id(p_path);
	}
#endif
	return id;
}

Dictionary GDRESettings::get_uid_cache() const {
	Dictionary ret;
	path_to_uid.for_each([&](const ParallelFlatHashMap<String, ResourceUID::ID>::value_type &e) {
		ret[e.first] = e.second;
	});
	return ret;
}

constexpr const char *GAME_NAME_SETTING_4x = "application/config/name";
constexpr const char *GAME_NAME_SETTING_2x = "application/name";

String GDRESettings::get_game_name() const {
	String game_name;
	if (is_project_config_loaded()) {
		game_name = current_project->pcfg->get_setting(get_ver_major() <= 2 ? GAME_NAME_SETTING_2x : GAME_NAME_SETTING_4x, "");
	}
	if (game_name.is_empty() && is_pack_loaded()) {
		game_name = get_pack_path().get_file().get_basename();
	}
	return game_name;
}

String GDRESettings::get_game_app_version() const {
	if (!is_pack_loaded()) {
		return "";
	}
	if (is_project_config_loaded()) {
		const char *version_setting = get_ver_major() <= 3 ? "application/version" : "application/config/version";
		return current_project->pcfg->get_setting(version_setting, current_project->app_version);
	}
	return current_project->app_version;
}

Error GDRESettings::load_pack_gdscript_cache(bool p_reset) {
	if (!is_pack_loaded()) {
		return ERR_UNAVAILABLE;
	}
	if (p_reset) {
		reset_gdscript_cache();
	}

	auto cache_file = get_loaded_pack_data_dir().path_join("global_script_class_cache.cfg");
	if (!FileAccess::exists(cache_file)) {
		return ERR_FILE_NOT_FOUND;
	}
	Array global_class_list;
	Ref<ConfigFileCompat> cf;
	cf.instantiate();
	if (cf->load(cache_file) == OK) {
		global_class_list = cf->get_value("", "list", Array());
	} else {
		return ERR_FILE_CANT_READ;
	}

	for (int i = 0; i < global_class_list.size(); i++) {
		Dictionary d = global_class_list[i];
		if (d.is_empty() || !d.has("path")) {
			continue;
		}
		String path = d["path"];
		script_cache[path] = d;
	}
	return OK;
}

namespace {
struct ScriptCacheTask {
	Ref<RegEx> steam_plugin_regex;
	struct ScriptCacheTaskToken {
		String orig_path;
		bool is_gdscript;
		Dictionary d;
		Ref<Script> script;
		bool uses_steam = false;
	};

	String get_description(int i, ScriptCacheTaskToken *tokens) const {
		return tokens[i].orig_path;
	}

	void do_task(int i, ScriptCacheTaskToken *tokens) {
		Ref<Script> script = ResourceCompatLoader::custom_load(tokens[i].orig_path, "", ResourceInfo::LoadType::REAL_LOAD, nullptr, false, ResourceFormatLoader::CACHE_MODE_REPLACE);
		if (script.is_valid()) {
			tokens[i].script = script;

			if (tokens[i].is_gdscript) {
				String source = script->get_source_code();
				if (steam_plugin_regex->search(source).is_valid()) {
					tokens[i].uses_steam = true;
				}
			}
			auto global_name = script->get_global_name();

			if (global_name.is_empty()) {
				return;
			}

			Ref<FakeScript> fake_script = script;
			String icon_path;
			if (fake_script.is_valid()) {
				tokens[i].d.set("base", fake_script->get_direct_base_type());
				icon_path = fake_script->get_icon_path();
			} else {
				auto base_script = script->get_base_script();
				if (base_script.is_valid()) {
					tokens[i].d.set("base", base_script->get_global_name());
				} else {
					tokens[i].d.set("base", script->get_instance_base_type());
				}
			}
			tokens[i].d.set("class", script->get_global_name());
			tokens[i].d.set("icon", icon_path);
			tokens[i].d.set("is_abstract", script->is_abstract());
			tokens[i].d.set("is_tool", script->is_tool());
			tokens[i].d.set("language", tokens[i].is_gdscript ? SNAME("GDScript") : SNAME("C#"));
			tokens[i].d.set("path", tokens[i].orig_path);
		}
	}
};
} //namespace

void GDRESettings::_ensure_script_cache_complete() {
	Vector<String> filters;
	if (has_loaded_dotnet_assembly()) {
		filters.push_back("*.cs");
	}
	if (get_bytecode_revision() != 0) {
		filters.append_array({ "*.gd", "*.gdc", "*.gde" });
	}
	if (filters.is_empty()) {
		return;
	}
	cached_scripts.clear();
	auto script_paths = get_file_list(filters);
	Vector<ScriptCacheTask::ScriptCacheTaskToken> tokens;
	for (auto &path : script_paths) {
		auto ext = path.get_extension().to_lower();
		bool bytecode_script = ext == "gdc" || ext == "gde";
		bool is_gdscript = ext == "gd" || bytecode_script;
		String orig_path = bytecode_script ? path.get_basename() + ".gd" : path;
		tokens.push_script_path ? tokens.push_back(ScriptCacheTask::ScriptCacheTaskToken{ orig_path, is_gdscript, {} }) : tokens.push_back(ScriptCacheTask::ScriptCacheTaskToken{ orig_path, is_gdscript, {} });
	}
	// Corrected cleanup block completion for the file tail
	if (tokens.size() == 0) {
		return;
	}

	GDRELogger::set_silent_errors(true);
	ScriptCacheTask task;
	task.steam_plugin_regex = RegEx::create_from_string("\\bSteam(?:(?:\\.(?:get_steam_init_result|STEAM_API_INIT_RESULT_OK|steamInit))|AppId)");
	if (tokens.size() > 50) {
		TaskManager::get_singleton()->run_multithreaded_group_task(
				&task,
				&ScriptCacheTask::do_task,
				tokens.ptrw(),
				tokens.size(),
				&ScriptCacheTask::get_description,
				"GDRESettings::load_pack_gdscript_cache",
				RTR("Loading GDScript cache..."),
				false);
	} else {
		for (int i = 0; i < tokens.size(); i++) {
			task.do_task(i, tokens.ptrw());
		}
	}
	cached_scripts.reserve(tokens.size());
	GDRELogger::set_silent_errors(false);
	for (int i = 0; i < tokens.size(); i++) {
		if (tokens[i].uses_steam) {
			current_project->detected_godotsteam_usage = true;
		}
		if (tokens[i].script.is_valid()) {
			cached_scripts.push_back(tokens[i].script);
		}
		if (!tokens[i].d.is_empty()) {
#ifdef DEBUG_ENABLED
			if (script_cache.has(tokens[i].orig_path)) {
				String err_msg = "";
				if (script_cache[tokens[i].orig_path].size() > tokens[i].d.size()) {
					err_msg += vformat("\tSizes are different: %d vs. %d\n", script_cache[tokens[i].orig_path].size(), tokens[i].d.size());
				}
				for (auto &E : script_cache[tokens[i].orig_path]) {
					if (!tokens[i].d.has(E.key)) {
						err_msg += vformat("\tKey '%s' not found in new dictionary\n", E.key);
					} else if (E.value != tokens[i].d[E.key]) {
						err_msg += vformat("\tKey '%s' value differs: %s vs. %s\n", E.key, E.value, tokens[i].d[E.key]);
					}
				}
				if (!err_msg.is_empty()) {
					print_verbose(vformat("Script cache entry differs for %s:\n%s", tokens[i].orig_path, err_msg));
				}
			}
#endif
			script_cache[tokens[i].orig_path] = tokens[i].d;
		}
	}
}

void GDRESettings::reset_gdscript_cache() {
	script_cache.clear();
	cached_scripts.clear();
}
