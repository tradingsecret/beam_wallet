#pragma once

#include "core/common.h"
#include "core/ecc_native.h"

struct sqlite3;
struct Nonce;

namespace beam
{
    struct Coin
    {
        enum Status
        {
            Unconfirmed,
            Unspent,
            Locked,
            Spent
        };

        Coin();
        Coin(uint64_t id, const ECC::Amount& amount, Status status = Coin::Unspent, const Height& height = 0, KeyType keyType = KeyType::Kernel);

        uint64_t m_id;
        ECC::Amount m_amount;
        Status m_status;
        Height m_height;
        KeyType m_key_type;
    };

    struct IKeyChain
    {
        using Ptr = std::shared_ptr<IKeyChain>;
        virtual ~IKeyChain() {}

        virtual uint64_t getNextID() = 0;

        virtual ECC::Scalar::Native calcKey(const beam::Coin& coin) const = 0;

        virtual std::vector<beam::Coin> getCoins(const ECC::Amount& amount, bool lock = true) = 0;
        virtual void store(const beam::Coin& coin) = 0;
        virtual void store(const std::vector<beam::Coin>& coins) = 0;
        virtual void update(const std::vector<beam::Coin>& coins) = 0;
        virtual void remove(const std::vector<beam::Coin>& coins) = 0;

		virtual void visit(std::function<bool(const beam::Coin& coin)> func) = 0;

		virtual void setVarRaw(const char* name, const void* data, int size) = 0;
		virtual int getVarRaw(const char* name, void* data) const = 0;

		template <typename Var>
		void setVar(const char* name, const Var& var)
		{
			setVarRaw(name, &var, sizeof(var));
		}

		template <typename Var>
		bool getVar(const char* name, Var& var)
		{
			return getVarRaw(name, &var) == sizeof(var);
		}
	};

    struct Keychain : IKeyChain
    {
        static Ptr init(const std::string& password);
        static Ptr open(const std::string& password);
        static const char* getName();

        Keychain(const std::string& pass);
        ~Keychain();

        uint64_t getNextID() override;
        ECC::Scalar::Native calcKey(const beam::Coin& coin) const override;
        std::vector<beam::Coin> getCoins(const ECC::Amount& amount, bool lock = true) override;
        void store(const beam::Coin& coin) override;
        void store(const std::vector<beam::Coin>& coins) override;
        void update(const std::vector<beam::Coin>& coins) override;
        void remove(const std::vector<beam::Coin>& coins) override;
		void visit(std::function<bool(const beam::Coin& coin)> func) override;

		void setVarRaw(const char* name, const void* data, int size) override;
		int getVarRaw(const char* name, void* data) const override;

    private:

        sqlite3* _db;
        std::shared_ptr<Nonce> _nonce;
        ECC::Kdf m_kdf;
    };
}
