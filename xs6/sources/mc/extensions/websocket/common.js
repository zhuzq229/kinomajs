/*
 *     Copyright (C) 2010-2016 Marvell International Ltd.
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

export function hash(key) {
	const {SHA1} = require.weak('crypt');
	let sha1 = new SHA1();
	sha1.update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	let digest = sha1.close();

	const {encode} = require.weak('bin');
	return encode(digest);
}

export default {
	hash,
};

