/*
 * Tenant.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_TENANT_H
#define FDBCLIENT_TENANT_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbclient/VersionedMap.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbrpc/TenantInfo.h"
#include "flow/flat_buffers.h"

typedef StringRef TenantNameRef;
typedef Standalone<TenantNameRef> TenantName;
typedef StringRef TenantGroupNameRef;
typedef Standalone<TenantGroupNameRef> TenantGroupName;

// Represents the various states that a tenant could be in.
// In a standalone cluster, a tenant should only ever be in the READY state.
// In a metacluster, a tenant on the management cluster could be in the other states while changes are applied to the
// data cluster.
//
// REGISTERING - the tenant has been created on the management cluster and is being created on the data cluster
// READY - the tenant has been created on both clusters, is active, and is consistent between the two clusters
// REMOVING - the tenant has been marked for removal and is being removed on the data cluster
// UPDATING_CONFIGURATION - the tenant configuration has changed on the management cluster and is being applied to the
//                          data cluster
// ERROR - currently unused
//
// A tenant in any configuration is allowed to be removed. Only tenants in the READY or UPDATING_CONFIGURATION phases
// can have their configuration updated. A tenant must not exist or be in the REGISTERING phase to be created.
//
// If an operation fails and the tenant is left in a non-ready state, re-running the same operation is legal. If
// successful, the tenant will return to the READY state.
enum class TenantState { REGISTERING, READY, RENAMING_FROM, RENAMING_TO, REMOVING, UPDATING_CONFIGURATION, ERROR };

// Represents the lock state the tenant could be in.
// Can be used in conjunction with the other tenant states above.
// A field will be present in the map entry but is currently unused.
enum class TenantLockState { UNLOCKED, READ_ONLY, LOCKED };

struct TenantMapEntry {
	constexpr static FileIdentifier file_identifier = 12247338;

	static Key idToPrefix(int64_t id);
	static int64_t prefixToId(KeyRef prefix);

	static std::string tenantStateToString(TenantState tenantState);
	static TenantState stringToTenantState(std::string stateStr);

	static std::string tenantLockStateToString(TenantLockState tenantState);
	static TenantLockState stringToTenantLockState(std::string stateStr);

	int64_t id = -1;
	Key prefix;
	TenantState tenantState = TenantState::READY;
	TenantLockState tenantLockState = TenantLockState::UNLOCKED;
	Optional<TenantGroupName> tenantGroup;
	bool encrypted = false;
	Optional<ClusterName> assignedCluster;
	Optional<TenantName> renamePair;
	int64_t configurationSequenceNum = 0;

	constexpr static int PREFIX_SIZE = sizeof(id);

	TenantMapEntry();
	TenantMapEntry(int64_t id, TenantState tenantState, bool encrypted);
	TenantMapEntry(int64_t id, TenantState tenantState, Optional<TenantGroupName> tenantGroup, bool encrypted);

	void setId(int64_t id);
	std::string toJson(int apiVersion) const;

	bool matchesConfiguration(TenantMapEntry const& other) const;
	void configure(Standalone<StringRef> parameter, Optional<Value> value);

	Value encode() const { return ObjectWriter::toValue(*this, IncludeVersion()); }
	static TenantMapEntry decode(ValueRef const& value) {
		return ObjectReader::fromStringRef<TenantMapEntry>(value, IncludeVersion());
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar,
		           id,
		           tenantState,
		           tenantLockState,
		           tenantGroup,
		           encrypted,
		           assignedCluster,
		           renamePair,
		           configurationSequenceNum);
		if constexpr (Ar::isDeserializing) {
			if (id >= 0) {
				prefix = idToPrefix(id);
			}
			ASSERT(tenantState >= TenantState::REGISTERING && tenantState <= TenantState::ERROR);
		}
	}
};

struct TenantGroupEntry {
	constexpr static FileIdentifier file_identifier = 10764222;

	Optional<ClusterName> assignedCluster;

	TenantGroupEntry() = default;
	TenantGroupEntry(Optional<ClusterName> assignedCluster) : assignedCluster(assignedCluster) {}

	Value encode() { return ObjectWriter::toValue(*this, IncludeVersion()); }
	static TenantGroupEntry decode(ValueRef const& value) {
		return ObjectReader::fromStringRef<TenantGroupEntry>(value, IncludeVersion());
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, assignedCluster);
	}
};

struct TenantTombstoneCleanupData {
	constexpr static FileIdentifier file_identifier = 3291339;

	// All tombstones have been erased up to and including this id.
	// We should not generate new tombstones at IDs equal to or older than this.
	int64_t tombstonesErasedThrough = -1;

	// The version at which we will next erase tombstones.
	Version nextTombstoneEraseVersion = invalidVersion;

	// When we reach the nextTombstoneEraseVersion, we will erase tombstones up through this ID.
	int64_t nextTombstoneEraseId = -1;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, tombstonesErasedThrough, nextTombstoneEraseVersion, nextTombstoneEraseId);
	}
};

struct TenantMetadataSpecification {
	Key subspace;

	KeyBackedObjectMap<TenantName, TenantMapEntry, decltype(IncludeVersion()), NullCodec> tenantMap;
	KeyBackedProperty<int64_t> lastTenantId;
	KeyBackedBinaryValue<int64_t> tenantCount;
	KeyBackedSet<int64_t> tenantTombstones;
	KeyBackedObjectProperty<TenantTombstoneCleanupData, decltype(IncludeVersion())> tombstoneCleanupData;
	KeyBackedSet<Tuple> tenantGroupTenantIndex;
	KeyBackedObjectMap<TenantGroupName, TenantGroupEntry, decltype(IncludeVersion()), NullCodec> tenantGroupMap;

	TenantMetadataSpecification(KeyRef prefix)
	  : subspace(prefix.withSuffix("tenant/"_sr)), tenantMap(subspace.withSuffix("map/"_sr), IncludeVersion()),
	    lastTenantId(subspace.withSuffix("lastId"_sr)), tenantCount(subspace.withSuffix("count"_sr)),
	    tenantTombstones(subspace.withSuffix("tombstones/"_sr)),
	    tombstoneCleanupData(subspace.withSuffix("tombstoneCleanup"_sr), IncludeVersion()),
	    tenantGroupTenantIndex(subspace.withSuffix("tenantGroup/tenantIndex/"_sr)),
	    tenantGroupMap(subspace.withSuffix("tenantGroup/map/"_sr), IncludeVersion()) {}
};

struct TenantMetadata {
	static inline TenantMetadataSpecification instance = TenantMetadataSpecification("\xff/"_sr);

	static inline auto& subspace = instance.subspace;
	static inline auto& tenantMap = instance.tenantMap;
	static inline auto& lastTenantId = instance.lastTenantId;
	static inline auto& tenantCount = instance.tenantCount;
	static inline auto& tenantTombstones = instance.tenantTombstones;
	static inline auto& tombstoneCleanupData = instance.tombstoneCleanupData;
	static inline auto& tenantGroupTenantIndex = instance.tenantGroupTenantIndex;
	static inline auto& tenantGroupMap = instance.tenantGroupMap;

	static inline Key tenantMapPrivatePrefix = "\xff"_sr.withSuffix(tenantMap.subspace.begin);
};

typedef VersionedMap<TenantName, TenantMapEntry> TenantMap;
typedef VersionedMap<Key, TenantName> TenantPrefixIndex;

#endif
