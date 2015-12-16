/*
 *     Copyright (C) 2010-2015 Marvell International Ltd.
 *     Copyright (C) 2002-2010 Kinoma, Inc.
 *
 *     Licensed under the Apache License, Version 2.0 (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

import * as FS from "fs";
import * as CMAKE from "cmake";

class Makefile extends CMAKE.Makefile {
	constructor(tree) {
		super(tree);
	}
	generateRules(tool, file, path) {
		file.line(`add_library(${this.name} STATIC \${${this.name}_SOURCES})`);
		file.line(`set_target_properties(${this.name} PROPERTIES STATIC_LIBRARY_FLAGS /LTCG)`);
		file.line(`add_dependencies(${this.name} FskManifest.xsa)`);
		file.line(`list(APPEND OBJECTS ${this.name})`);
		file.line(`set(OBJECTS \${OBJECTS} PARENT_SCOPE)`);
	}
};

class Manifest extends CMAKE.Manifest {
	constructor(tree) {
		super(tree);
	}
	get Makefile() {
		return Makefile;
	}
	getGenerator(tool) {
		let vs = null;
		let VisualStudioVariants = {
			10: "Visual Studio 10 2010",
			11: "Visual Studio 11 2012",
			12: "Visual Studio 12 2013",
			14: "Visual Studio 14 2015"
		};
		for (let key in VisualStudioVariants) {
			let version = key * 10;
			let comntools = process.getenv(`VS${version}COMNTOOLS`);
			if (comntools) {
				vs = VisualStudioVariants[key];
			}
		}

		if (vs) {
			if (tool.m64)
				return `${vs} Win64`;
			else
				return vs;
		} else {
			let envPath = process.getenv("PATH");
			let nmake = false;
			for (let dir of envPath.split(";")) {
				let path = tool.joinPath({ directory: dir, name: "nmake.exe"});
				let nmakeFound = FS.existsSync(path);
				if (nmakeFound) {
					nmake = true;
				}
			}
			if (nmake)
				return "NMake Makefiles";
			else
				throw new Error("Unable to find Visual Studio or NMake");
		}
	}
	getIDEGenerator(tool) {
		let vs =  this.getGenerator(tool);
		if (vs == "NMake Makefiles")
			throw new Error("Unable to determine your Visual Studio version");
		else
			return vs;
	}
	openIDE(tool, path) {
		process.then("cmd.exe", "/c", `${path}${tool.slash}fsk.sln`);
	}
	getPlatformVariables(tool, tmp, bin) {
		var parts = tool.splitPath(tool.manifestPath);
		var resource = parts.directory + "\\win\\resource.rc";
		var path = process.debug ? "$(F_HOME)\\xs6\\bin\\win\\debug" : "$(F_HOME)\\xs6\\bin\\win\\release";
		return {
			APP_DIR: `${tool.outputPath}/bin/${tool.platform}/$<CONFIG>/${tool.application}`,
			TMP_DIR: tmp,
			
			APP_NAME: tool.application,
			RESOURCE: FS.existsSync(resource) ? resource : "$(F_HOME)\\kinoma\\kpr\\cmake\\win\\resource.rc",
			// FskPlatform.mk
			BUILD_TMP: tmp,
		};
	}
	getTargetRules(tool) {
		return `LIST(APPEND SOURCES \${RESOURCE})
add_executable(\${APP_NAME} WIN32 \${SOURCES} \${FskPlatform_SOURCES})
target_link_libraries(\${APP_NAME} \${LIBRARIES} \${OBJECTS})

add_custom_target(
	Assemble
	ALL
	COMMAND \${CMAKE_COMMAND} -E make_directory \${APP_DIR}
	COMMAND \${CMAKE_COMMAND} -E copy_directory \${RES_DIR}/ \${APP_DIR}
	COMMAND \${CMAKE_COMMAND} -E copy_directory \${TMP_DIR}/app \$<TARGET_FILE_DIR:\${APP_NAME}>
	COMMAND \${CMAKE_COMMAND} -E make_directory \${APP_DIR}
	COMMAND \${CMAKE_COMMAND} -E copy \$<TARGET_FILE:\${APP_NAME}> \${APP_DIR}
	DEPENDS \${APP_NAME} FskManifest.xsa
	)
`;
	}
};

export default Manifest;
