#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "QoderDesktopAuth.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
    void Require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }

    // All values here are synthetic: no real token or application key is copied.
    void WriteFixture(const std::filesystem::path& folder, const std::string& token)
    {
        std::filesystem::create_directories(folder);
        std::vector<unsigned char> key(32, 42);
        DATA_BLOB input{ static_cast<DWORD>(key.size()), key.data() }, wrapped{};
        Require(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &wrapped) != FALSE, "protect fixture key");
        std::vector<unsigned char> encoded{ 'D', 'P', 'A', 'P', 'I' };
        encoded.insert(encoded.end(), wrapped.pbData, wrapped.pbData + wrapped.cbData);
        LocalFree(wrapped.pbData);
        DWORD base64_size{};
        Require(CryptBinaryToStringA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64_size) != FALSE, "size fixture base64");
        std::string base64(base64_size, 0);
        Require(CryptBinaryToStringA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.data(), &base64_size) != FALSE, "encode fixture key");
        base64.resize(base64_size);
        if (!base64.empty() && base64.back() == 0) base64.pop_back();
        std::ofstream(folder / L"Local State") << "{\"os_crypt\":{\"encrypted_key\":\"" << base64 << "\"}}";

        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_KEY_HANDLE aes{};
        Require(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)), "open fixture AES");
        Require(BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0)), "fixture AES mode");
        Require(BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &aes, nullptr, 0, key.data(), static_cast<ULONG>(key.size()), 0)), "fixture AES key");
        const std::string plain = "{\"token\":\"" + token + "\",\"expiresAt\":2000000000000}";
        unsigned char nonce[12]{}, tag[16]{};
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce; info.cbNonce = sizeof(nonce); info.pbTag = tag; info.cbTag = sizeof(tag);
        std::vector<unsigned char> cipher(plain.size());
        ULONG written{};
        const bool encrypted = BCRYPT_SUCCESS(BCryptEncrypt(aes, reinterpret_cast<PUCHAR>(const_cast<char*>(plain.data())),
            static_cast<ULONG>(plain.size()), &info, nullptr, 0, cipher.data(), static_cast<ULONG>(cipher.size()), &written, 0));
        BCryptDestroyKey(aes); BCryptCloseAlgorithmProvider(algorithm, 0);
        Require(encrypted, "encrypt fixture token");
        std::ofstream auth(folder / L"auth.v1.dat", std::ios::binary);
        auth.write("v10", 3); auth.write(reinterpret_cast<const char*>(nonce), sizeof(nonce));
        auth.write(reinterpret_cast<const char*>(cipher.data()), written); auth.write(reinterpret_cast<const char*>(tag), sizeof(tag));
    }
}

int main(int argc, char** argv)
{
    using QoderDesktopAuth::Edition;
    wchar_t appdata[32768]{}, temp[32768]{};
    const DWORD original_size = GetEnvironmentVariableW(L"APPDATA", appdata, static_cast<DWORD>(std::size(appdata)));
    Require(original_size > 0 && original_size < std::size(appdata), "original APPDATA");
    Require(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) > 0, "temporary folder");
    const auto root = std::filesystem::absolute(std::filesystem::path(temp) /
        (L"QoderDesktopAuthTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    int result = 0;
    try
    {
        WriteFixture(root / L"com.qoder.app.stable", "synthetic-international");
        Require(SetEnvironmentVariableW(L"APPDATA", root.c_str()) != FALSE, "set fixture APPDATA");
        QoderDesktopAuth::Credential global, china;
        Require(QoderDesktopAuth::LoadDesktopCredential(global, Edition::International) && global.token == L"synthetic-international", "read international fixture");
        Require(!QoderDesktopAuth::LoadDesktopCredential(china, Edition::China), "QoderCN must not fall back to international credentials");
        WriteFixture(root / L"com.qodercn.app.stable", "synthetic-china");
        Require(QoderDesktopAuth::LoadDesktopCredential(china, Edition::China) && china.token == L"synthetic-china", "read China fixture");
        Require(QoderDesktopAuth::LoadDesktopCredential(global, Edition::International) && global.token == L"synthetic-international", "China fixture must not replace international account");
        Require(china.origin == L"com.qodercn.app.stable" && global.origin == L"com.qoder.app.stable", "distinct credential sources");
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
    SetEnvironmentVariableW(L"APPDATA", appdata);
    // Restrict cleanup to the unique fixture directory created directly inside TEMP.
    if (root.parent_path() == std::filesystem::absolute(temp) && root.filename().wstring().find(L"QoderDesktopAuthTests-") == 0)
        std::filesystem::remove_all(root);
    if (!result && argc > 1 && std::string(argv[1]) == "--live")
    {
        QoderDesktopAuth::Credential global, china;
        const bool global_ok = QoderDesktopAuth::LoadDesktopCredential(global, Edition::International);
        const bool china_ok = QoderDesktopAuth::LoadDesktopCredential(china, Edition::China);
        std::cout << "Live credentials: Qoder=" << global_ok << "; QoderCN=" << china_ok << '\n';
        result = global_ok && china_ok ? 0 : 1;
    }
    std::cout << (result ? "FAILED" : "PASSED") << ": Qoder / QoderCN DPAPI credentials and edition isolation\n";
    return result;
}
