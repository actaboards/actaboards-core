/*
 * Copyright (c) 2020-2023 Revolution Populi Limited, and contributors.
 * Copyright (c) 2024 ActaBoards contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <graphene/chain/types.hpp>
#include <graphene/db/generic_index.hpp>
#include <graphene/protocol/account.hpp>

#include <boost/multi_index/composite_key.hpp>

namespace graphene { namespace chain {

   class database;

   /**
    * @brief Room object - encrypted thread
    * @ingroup object
    * @ingroup protocol
    *
    * A room is a container for content_cards that share a common encryption key.
    * Only participants of the room can create content_cards in it.
    */
   class room_object : public graphene::db::abstract_object<room_object>
   {
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = room_object_type;

      account_id_type owner;       // Room owner (cannot be removed)
      string          name;        // Room name
      string          room_key;    // Encrypted room key (for owner)
      uint64_t        timestamp;   // Creation timestamp
      uint32_t        current_epoch = 0;  // Current key epoch
   };

   /**
    * @brief Room participant object
    * @ingroup object
    * @ingroup protocol
    *
    * Represents a participant in a room with their encrypted content key.
    */
   class room_participant_object : public graphene::db::abstract_object<room_participant_object>
   {
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = room_participant_object_type;

      room_id_type    room;           // Reference to room
      account_id_type participant;    // Participant account
      string          content_key;    // Room key encrypted for this participant
      uint64_t        timestamp;      // When added
   };

   // ============ Room Indexes ============

   struct by_owner;
   struct by_name;

   typedef multi_index_container<
      room_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_non_unique< tag<by_owner>,
            member< room_object, account_id_type, &room_object::owner >
         >,
         ordered_unique< tag<by_name>,
            composite_key< room_object,
               member< room_object, account_id_type, &room_object::owner>,
               member< room_object, string, &room_object::name>
            >
         >
      >
   > room_multi_index_type;

   typedef generic_index<room_object, room_multi_index_type> room_index;

   // ============ Room Participant Indexes ============

   struct by_room;
   struct by_participant;
   struct by_room_and_participant;

   typedef multi_index_container<
      room_participant_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_non_unique< tag<by_room>,
            member< room_participant_object, room_id_type, &room_participant_object::room >
         >,
         ordered_non_unique< tag<by_participant>,
            member< room_participant_object, account_id_type, &room_participant_object::participant >
         >,
         ordered_unique< tag<by_room_and_participant>,
            composite_key< room_participant_object,
               member< room_participant_object, room_id_type, &room_participant_object::room>,
               member< room_participant_object, account_id_type, &room_participant_object::participant>
            >
         >
      >
   > room_participant_multi_index_type;

   typedef generic_index<room_participant_object, room_participant_multi_index_type> room_participant_index;

   // ============ Room Key Epoch Object ============

   /**
    * @brief Room key epoch object - stores per-participant encrypted keys for each epoch
    * @ingroup object
    * @ingroup protocol
    */
   class room_key_epoch_object : public graphene::db::abstract_object<room_key_epoch_object>
   {
   public:
      static constexpr uint8_t space_id = protocol_ids;
      static constexpr uint8_t type_id  = room_key_epoch_object_type;

      room_id_type    room;
      uint32_t        epoch;          // Key epoch number
      account_id_type participant;    // Participant account
      string          content_key;    // Epoch key encrypted for this participant
   };

   // ============ Room Key Epoch Indexes ============

   struct by_room_and_epoch;
   struct by_room_epoch_participant;
   struct by_room_and_participant_epoch;

   typedef multi_index_container<
      room_key_epoch_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_non_unique< tag<by_room_and_epoch>,
            composite_key< room_key_epoch_object,
               member< room_key_epoch_object, room_id_type, &room_key_epoch_object::room>,
               member< room_key_epoch_object, uint32_t, &room_key_epoch_object::epoch>
            >
         >,
         ordered_unique< tag<by_room_epoch_participant>,
            composite_key< room_key_epoch_object,
               member< room_key_epoch_object, room_id_type, &room_key_epoch_object::room>,
               member< room_key_epoch_object, uint32_t, &room_key_epoch_object::epoch>,
               member< room_key_epoch_object, account_id_type, &room_key_epoch_object::participant>
            >
         >,
         ordered_non_unique< tag<by_room_and_participant_epoch>,
            composite_key< room_key_epoch_object,
               member< room_key_epoch_object, room_id_type, &room_key_epoch_object::room>,
               member< room_key_epoch_object, account_id_type, &room_key_epoch_object::participant>
            >
         >
      >
   > room_key_epoch_multi_index_type;

   typedef generic_index<room_key_epoch_object, room_key_epoch_multi_index_type> room_key_epoch_index;

} } // graphene::chain

MAP_OBJECT_ID_TO_TYPE(graphene::chain::room_object)
MAP_OBJECT_ID_TO_TYPE(graphene::chain::room_participant_object)
MAP_OBJECT_ID_TO_TYPE(graphene::chain::room_key_epoch_object)

FC_REFLECT_TYPENAME( graphene::chain::room_object )
FC_REFLECT_TYPENAME( graphene::chain::room_participant_object )
FC_REFLECT_TYPENAME( graphene::chain::room_key_epoch_object )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::room_object )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::room_participant_object )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::room_key_epoch_object )
