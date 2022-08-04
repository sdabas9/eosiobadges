#include <eosio/eosio.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>
#include <eosio/asset.hpp>
#include <atomic-interface.hpp>

using namespace std;
using namespace eosio;

# define BILLING_CONTRACT "orgbill"
# define ACCOUNT_PREFERENCES_CONTRACT "accountprefs"
# define ATOMIC_ASSETS_CONTRACT "atomicassets"

# define ATOMIC_ASSETS_CREATE_TEMPLATE_NOTIFICATION "atomicassets::lognewtempl"

# define ATOMIC_ASSETS_COLLECTION_NAME "openprofiles"
# define ATOMIC_ASSETS_SCHEMA_NAME "openschema"


CONTRACT profile : public contract {
  public:
    using contract::contract;

  struct authorization_details {
    bool is_authorized;
    name authorized_account;
  };

  struct badgedata_details {
    uint64_t badge_id;
    bool write_to_aa;
    uint32_t aa_template_id;
  };

  struct usecredit_args {
    name billed_org;
    uint64_t bytes_used;
  };
  
  struct createtemplate_args {
    name authorized_creator;
    name collection_name;
    name schema_name;
    bool transferable;
    bool burnable;
    uint32_t max_supply;
    ATTRIBUTE_MAP immutable_data;
  };

  struct mintaa_args {
    name authorized_creator;
    name collection_name;
    name schema_name;
    uint32_t template_id;
    name new_asset_owner;
    ATTRIBUTE_MAP immutable_data;
    ATTRIBUTE_MAP mutable_data;
    vector <asset> tokens_to_back;
  };

  struct checkallow_args {
    name org;
    name account;
  };

  ACTION del (name org);

  ACTION authcontract (name org, name trusted_contract);

  ACTION initbadge(name org, name badge, string ipfs, string details, bool write_to_aa);

  ACTION achievement (name org, name badge, name account, uint8_t count);

  [[eosio::on_notify(ATOMIC_ASSETS_CREATE_TEMPLATE_NOTIFICATION)]] void updatebadge(
    int32_t template_id,
    name authorized_creator,
    name collection_name,
    name schema_name,
    bool transferable,
    bool burnable,
    uint32_t max_supply,
    ATTRIBUTE_MAP immutable_data); 

  private:
    // scoped by org
    // stores trusted and adopted badge contracts.
    TABLE authorized {
      name trusted_contract;
      auto primary_key() const {return trusted_contract.value; }
    };
    typedef multi_index<name("authorized"), authorized> authorized_contracts_table;

    // scoped by org
    // stores metadata information for a badge issued by trusted/adopted badge contract
    TABLE badgedata {
      uint64_t badge_id;
      name contract;
      name badge;
      bool write_to_aa;
      uint32_t aa_template_id;
      string ipfs;
      string details;
      uint32_t rarity_counts;
      auto primary_key() const {return badge_id; }
      uint128_t contract_badge_key() const {
        return ((uint128_t) contract.value) << 64 | badge.value;
      }
    };
    typedef multi_index<name("badgedata"), badgedata,
    indexed_by<name("contractbadge"), const_mem_fun<badgedata, uint128_t, &badgedata::contract_badge_key>>    
    > badgedata_table;
    
    // scoped by org
    TABLE achievements {
      uint64_t id;
      name account;
      uint64_t badge_id;
      uint32_t count;
      auto primary_key() const {return id; }
      uint128_t acc_badge_key() const {
        return ((uint128_t) account.value) << 64 | badge_id;
      }
    };
    typedef multi_index<name("achievements"), achievements,
    indexed_by<name("accountbadge"), const_mem_fun<achievements, uint128_t, &achievements::acc_badge_key>>
    > achievements_table;


    badgedata_details get_update_badgedata_state (name org, name badge, name contract, uint8_t count) {
      badgedata_table _badgedata( _self, org.value );
      auto contract_badge_index = _badgedata.get_index<name("contractbadge")>();
      uint128_t contract_badge_key = ((uint128_t) contract.value) << 64 | badge.value;
      auto contract_badge_iterator = contract_badge_index.find (contract_badge_key);

      check(contract_badge_iterator != contract_badge_index.end(), "<org> has not created <badge> for <contract>");

      contract_badge_index.modify(contract_badge_iterator, get_self(), [&](auto& row){
        row.rarity_counts = row.rarity_counts + count;
      });

      return badgedata_details {
        .badge_id = contract_badge_iterator->badge_id,
        .write_to_aa = contract_badge_iterator->write_to_aa,
        .aa_template_id = contract_badge_iterator->aa_template_id
      };
    }


    authorization_details auth (name org) {
      authorized_contracts_table _authorized_contracts( _self, org.value );
      for(auto itr = _authorized_contracts.begin(); itr != _authorized_contracts.end(); ++itr ) {
        if (has_auth(itr->trusted_contract)) {
          return authorization_details {
            .is_authorized = true,
            .authorized_account = itr->trusted_contract
          };
        }
      }
      return authorization_details {
        .is_authorized = false
      };
    }

    void check_account_prefs (name org, name account) {
      action {
        permission_level{get_self(), name("active")},
        name(ACCOUNT_PREFERENCES_CONTRACT),
        name("checkallow"),
        checkallow_args {
          .org = org,
          .account = account }
      }.send();
    }
    
    void deduct_credit (name org, uint64_t bytes_used) {
      action {
        permission_level{get_self(), name("active")},
        name(BILLING_CONTRACT),
        name("usecredit"),
        usecredit_args {
          .billed_org = org,
          .bytes_used = bytes_used }
      }.send();
    }

    uint8_t write_native_achievement(name org, name account, uint64_t badge_id, uint8_t count) {
      uint8_t rows_inserted = 0;
      achievements_table _achievements( _self, org.value );
      auto account_badge_index = _achievements.get_index<name("accountbadge")>();
      uint128_t account_badge_key = ((uint128_t) account.value) << 64 | badge_id;
      auto account_badge_iterator = account_badge_index.find (account_badge_key);

      if(account_badge_iterator == account_badge_index.end()) {
        _achievements.emplace(get_self(), [&](auto& row){
          row.id = _achievements.available_primary_key();
          row.account = account;
          row.badge_id = badge_id;
          row.count = count;
        });
        rows_inserted = 1;
      } else {
        account_badge_index.modify(account_badge_iterator, get_self(), [&](auto& row){
          row.count = row.count + count;
        });
        rows_inserted = 0;
      }
      return rows_inserted;
    }

    uint8_t write_aa_achievement(name account, bool write_to_aa, uint32_t template_id, uint8_t count) {
      uint8_t rows_inserted = 0;
      if(write_to_aa) {
        std::map <std::string, ATOMIC_ATTRIBUTE> immutable_data;
        std::map <std::string, ATOMIC_ATTRIBUTE> mutable_data;
        vector<asset> tokens_to_back;
        for (auto i = 0; i < count; i++) {
          action {
            permission_level{get_self(), name("active")},
            name(ATOMIC_ASSETS_CONTRACT),
            name("mintasset"),
            mintaa_args {
              .authorized_creator = get_self(),
              .collection_name = name(ATOMIC_ASSETS_COLLECTION_NAME),
              .schema_name = name(ATOMIC_ASSETS_SCHEMA_NAME),
              .template_id = template_id,
              .new_asset_owner = account,
              .immutable_data = immutable_data,
              .mutable_data = mutable_data,
              .tokens_to_back = tokens_to_back}
          }.send();
        }
        rows_inserted = count;
      } 
      return rows_inserted;
    }

    uint64_t achievement_bytes () {
      return 64*4 + 32;
    }

    uint8_t write_native_badgedata(name org, name authorized_account, name badge, string ipfs, string details, bool write_to_aa) {

      badgedata_table _badgedata( _self, org.value );
      auto contract_badge_index = _badgedata.get_index<name("contractbadge")>();
      uint128_t contract_badge_key = ((uint128_t) authorized_account.value) << 64 | badge.value;
      auto contract_badge_iterator = contract_badge_index.find (contract_badge_key);
      check(contract_badge_iterator == contract_badge_index.end(), "<badge> already exists for <contract> ");

      _badgedata.emplace(get_self(), [&](auto& row){
        row.badge_id = _badgedata.available_primary_key();
        row.contract = authorized_account;
        row.badge = badge;
        row.ipfs = ipfs;
        row.details = details;
        row.rarity_counts = 0;
        row.write_to_aa = write_to_aa;
      });

      return 1;
    }

    uint8_t write_aa_template(name org, name authorized_account, name badge, string ipfs, string details, bool write_to_aa) {
      if (write_to_aa) {
        ATTRIBUTE_MAP mdata = {};
        mdata["org"] = string(org.to_string());
        mdata["badge"] = string(badge.to_string());
        mdata["contract"] = string(authorized_account.to_string());

        action {
        permission_level{get_self(), name("active")},
        name(ATOMIC_ASSETS_CONTRACT),
        name("createtempl"),
        createtemplate_args {
          .authorized_creator = get_self(),
          .collection_name = name(ATOMIC_ASSETS_COLLECTION_NAME),
          .schema_name = name(ATOMIC_ASSETS_SCHEMA_NAME),
          .transferable = false,
          .burnable = false,
          .max_supply = 0,
          .immutable_data = mdata}
        }.send();
        return 1;
      }
      return 0;
    }

    uint64_t init_bytes(string details, string ipfs) {
      return 2*(3*64 + 2*32 + details.length() + ipfs.length());  
    }

};
