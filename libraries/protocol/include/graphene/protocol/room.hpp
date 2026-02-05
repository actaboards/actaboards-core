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

#include <graphene/protocol/base.hpp>
#include <graphene/protocol/asset.hpp>
#include <graphene/protocol/authority.hpp>

namespace graphene { namespace protocol {

   /**
    * @brief Create a room (encrypted thread)
    *
    * This operation creates a new room with an encrypted room_key.
    * The owner is automatically added as the first participant.
    */
   struct room_create_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee = 20 * GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset           fee;
      account_id_type owner;      // Room owner
      string          name;       // Room name (max 256 chars)
      string          room_key;   // Encrypted room key (for owner)

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         a.insert( owner );
      }
   };

   /**
    * @brief Update room name (owner only)
    *
    * This operation allows the owner to change the room name.
    */
   struct room_update_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee = 5 * GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset           fee;
      account_id_type owner;
      room_id_type    room;
      string          name;       // New room name

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         a.insert( owner );
      }
   };

   /**
    * @brief Add participant to room
    *
    * This operation adds a new participant to the room with their encrypted content_key.
    * Only the room owner can add participants.
    */
   struct room_add_participant_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee = 5 * GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset           fee;
      account_id_type owner;           // Only owner can add participants
      room_id_type    room;
      account_id_type participant;     // Participant to add
      string          content_key;     // Room key encrypted for participant

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         a.insert( owner );
      }
   };

   /**
    * @brief Remove participant from room
    *
    * This operation removes a participant from the room.
    * Only the room owner can remove participants.
    * The owner cannot be removed from the room.
    */
   struct room_remove_participant_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee = GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset                       fee;
      account_id_type             owner;           // Only owner can remove
      room_participant_id_type    participant_id;  // Participant object to remove

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         a.insert( owner );
      }
   };

} } // graphene::protocol

FC_REFLECT( graphene::protocol::room_create_operation::fee_parameters_type, (fee) )
FC_REFLECT( graphene::protocol::room_create_operation,
            (fee)(owner)(name)(room_key)
          )

FC_REFLECT( graphene::protocol::room_update_operation::fee_parameters_type, (fee) )
FC_REFLECT( graphene::protocol::room_update_operation,
            (fee)(owner)(room)(name)
          )

FC_REFLECT( graphene::protocol::room_add_participant_operation::fee_parameters_type, (fee) )
FC_REFLECT( graphene::protocol::room_add_participant_operation,
            (fee)(owner)(room)(participant)(content_key)
          )

FC_REFLECT( graphene::protocol::room_remove_participant_operation::fee_parameters_type, (fee) )
FC_REFLECT( graphene::protocol::room_remove_participant_operation,
            (fee)(owner)(participant_id)
          )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::room_create_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::room_update_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::room_add_participant_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::room_remove_participant_operation )
