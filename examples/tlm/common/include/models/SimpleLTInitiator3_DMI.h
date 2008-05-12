/*****************************************************************************

  The following code is derived, directly or indirectly, from the SystemC
  source code Copyright (c) 1996-2007 by all Contributors.
  All Rights reserved.

  The contents of this file are subject to the restrictions and limitations
  set forth in the SystemC Open Source License Version 3.0 (the "License");
  You may not use this file except in compliance with such restrictions and
  limitations. You may obtain instructions on how to receive a copy of the
  License at http://www.systemc.org/. Software distributed by Contributors
  under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
  ANY KIND, either express or implied. See the License for the specific
  language governing rights and limitations under the License.

 *****************************************************************************/

#ifndef __SIMPLE_LT_INITIATOR3_DMI_H__
#define __SIMPLE_LT_INITIATOR3_DMI_H__

#include "tlm.h"
#include "simple_initiator_socket.h"
#include <systemc>
#include <cassert>
#include <iostream>
#include <map>

class SimpleLTInitiator3_dmi : public sc_core::sc_module
{
public:
  typedef tlm::tlm_generic_payload transaction_type;
  typedef tlm::tlm_dmi             dmi_type;
  typedef tlm::tlm_phase           phase_type;
  typedef tlm::tlm_sync_enum       sync_enum_type;
  typedef SimpleInitiatorSocket<>  initiator_socket_type;

public:
  initiator_socket_type socket;

public:
  SC_HAS_PROCESS(SimpleLTInitiator3_dmi);
  SimpleLTInitiator3_dmi(sc_core::sc_module_name name,
                  unsigned int nrOfTransactions = 0x5,
                  unsigned int baseAddress = 0x0) :
    sc_core::sc_module(name),
    socket("socket"),
    mNrOfTransactions(nrOfTransactions),
    mBaseAddress(baseAddress),
    mTransactionCount(0)
  {
    mDMIDataReads.first.set_start_address(1);
    mDMIDataReads.first.set_end_address(0);
    mDMIDataWrites.first.set_start_address(1);
    mDMIDataWrites.first.set_end_address(0);

    REGISTER_INVALIDATEDMI(socket, invalidate_direct_mem_ptr);

    // Initiator thread
    SC_THREAD(run);
  }

  bool initTransaction(transaction_type& trans)
  {
    if (mTransactionCount < mNrOfTransactions) {
      trans.set_address(mBaseAddress + 4*mTransactionCount);
      mData = mTransactionCount;
      trans.set_data_ptr(reinterpret_cast<unsigned char*>(&mData));
      trans.set_command(tlm::TLM_WRITE_COMMAND);

    } else if (mTransactionCount < 2 * mNrOfTransactions) {
      trans.set_address(mBaseAddress + 4*(mTransactionCount-mNrOfTransactions));
      mData = 0;
      trans.set_data_ptr(reinterpret_cast<unsigned char*>(&mData));
      trans.set_command(tlm::TLM_READ_COMMAND);

    } else {
      return false;
    }

    ++mTransactionCount;
    return true;
  }

  void logStartTransation(transaction_type& trans)
  {
    if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {
      std::cout << name() << ": Send write request: A = 0x"
                << std::hex << (unsigned int)trans.get_address()
                << ", D = 0x" << mData << std::dec
                << " @ " << sc_core::sc_time_stamp() << std::endl;
      
    } else {
      std::cout << name() << ": Send read request: A = 0x"
                << std::hex << (unsigned int)trans.get_address() << std::dec
                << " @ " << sc_core::sc_time_stamp() << std::endl;
    }
  }

  void logEndTransaction(transaction_type& trans)
  {
    if (trans.get_response_status() != tlm::TLM_OK_RESPONSE) {
      std::cout << name() << ": Received error response @ "
                << sc_core::sc_time_stamp() << std::endl;

    } else {
      std::cout << name() <<  ": Received ok response";
      if (trans.get_command() == tlm::TLM_READ_COMMAND) {
        std::cout << ": D = 0x" << std::hex << mData << std::dec;
      }
      std::cout << " @ " << sc_core::sc_time_stamp() << std::endl;
    }
  }

  std::pair<dmi_type, bool>& getDMIData(const transaction_type& trans)
  {
    if (trans.get_command() == tlm::TLM_READ_COMMAND) {
      return mDMIDataReads;

    } else { // WRITE
      return mDMIDataWrites;
    }
  }

  void run()
  {
    transaction_type trans;
    phase_type phase;
    sc_core::sc_time t;
    
    while (initTransaction(trans)) {
      // Create transaction and initialise phase and t
      phase = tlm::BEGIN_REQ;
      t = sc_core::SC_ZERO_TIME;

      logStartTransation(trans);

      ///////////////////////////////////////////////////////////
      // DMI handling:
      // We do *not* use the DMI hint to check if it makes sense to ask for
      // DMI pointers. So the pattern is:
      // - if the address is not covered by a DMI region try to acquire DMI
      //   pointers
      // - if we have a DMI pointer, do the DMI "transaction"
      // - otherwise fall back to a normal transaction
      ///////////////////////////////////////////////////////////

      std::pair<dmi_type, bool>& dmi_data = getDMIData(trans);

      // Check if we need to acquire a DMI pointer
      if((trans.get_address() < dmi_data.first.get_start_address()) ||
         (trans.get_address() > dmi_data.first.get_end_address()) )
      {
          dmi_data.second =
            socket->get_direct_mem_ptr(trans,
                                       dmi_data.first);
      }
      // Do DMI "transaction" if we have a valid region
      if (dmi_data.second &&
          (trans.get_address() >= dmi_data.first.get_start_address()) &&
          (trans.get_address() <= dmi_data.first.get_end_address()) )
      {
          // We can handle the data here. As the logEndTransaction is assuming
          // something to happen in the data structure, we really need to
          // do this:
          trans.set_response_status(tlm::TLM_OK_RESPONSE);

          sc_dt::uint64 tmp = trans.get_address() - dmi_data.first.get_start_address();
          if (trans.get_command() == tlm::TLM_WRITE_COMMAND)
          {
              *(unsigned int*)&dmi_data.first.get_dmi_ptr()[tmp] = mData;
          }
          else
          {
              mData = *(unsigned int*)&dmi_data.first.get_dmi_ptr()[tmp];
          }
          
          // Do the wait immediately. Note that doing the wait here eats almost
          // all the performance anyway, so we only gain something if we're
          // using temporal decoupling.
          if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {
            wait(dmi_data.first.get_write_latency());

          } else {
            wait(dmi_data.first.get_read_latency());
          }
      }
      else // we need a full transaction
      {
          switch (socket->nb_transport(trans, phase, t)) {
          case tlm::TLM_COMPLETED:
              // Transaction Finished, wait for the returned delay
              wait(t);
              break;
              
          case tlm::TLM_ACCEPTED:
          case tlm::TLM_UPDATED:
              // Transaction not yet finished, wait for the end of it
              wait(socket.getEndEvent());
              break;

          case tlm::TLM_REJECTED:
            // FIXME: Not supported (wait and retry same transaction)
          default:
            assert(0); exit(1);
          };
      }

      logEndTransaction(trans);
    }
    sc_core::sc_stop();
    wait();

  }

  // Invalidate DMI pointer(s)
  void invalidate_direct_mem_ptr(sc_dt::uint64 start_range,
                                 sc_dt::uint64 end_range)
  {
    // FIXME: probably faster to always invalidate everything?
    if (start_range <= mDMIDataReads.first.get_end_address ()&&
        end_range >= mDMIDataReads.first.get_start_address()) {
        mDMIDataReads.second = false;
    }
    if (start_range <= mDMIDataWrites.first.get_end_address ()&&
        end_range >= mDMIDataWrites.first.get_start_address()) {
      mDMIDataWrites.second = false;
    }
  }

private:
  std::pair<dmi_type, bool> mDMIDataReads;
  std::pair<dmi_type, bool> mDMIDataWrites;
  
  sc_core::sc_event mEndEvent;
  unsigned int mNrOfTransactions;
  unsigned int mBaseAddress;
  unsigned int mTransactionCount;
  unsigned int mData;
};

#endif
