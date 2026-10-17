# 造许可测试固定件：tests/unit/LicenseFixtures.h（并在 -WriteKeysHeader 时同步
# src/license/LicenseKeys.h 的公钥表）。
#
# 这里嵌的是**一次性开发私钥**（tools/make_license_key.ps1 生成的 lic-2026a /
# lic-2027a 两把），只用来给测试许可签名；正式私钥在离线环境，绝不进仓库。
# 固定件里的时间戳与字段值被 tests/unit/test_main.cpp 逐条断言，
# 改动它们必须同步改那一段单测。
#
# 用法：powershell -NoProfile -File tools\make_license_fixtures.ps1 [-WriteKeysHeader]
param([switch]$WriteKeysHeader)
$ErrorActionPreference='Stop'

# ---- 开发钥匙（与 build\b3_key_a.txt 一致；换钥匙时先跑 make_license_key.ps1）----
$keyA = @'
keyId = lic-2026a
public = UlNBMQAIAAADAAAAAAEAAAAAAAAAAAAAAQAB3mk4CIklDmBqCUfhnRw9qUEWr5/fy3Y46+GQsVPE+UyeFnP1O0Vaa+20l+l7mqd2htRZYs9suVL+eczWRUgHf1BsJsUZi9IL957zS8D5zJ57yEyoGO5y+WpPpnAXg/bnYb1i7hEc9QAK6QC2ZLfUK0ab4pRN9mQcpuzuX8EpIMl7Oc1UhNGF+JWevvoMl12ESLjBEWyTCz52Zf0vbnNICw0qXBOcDCPjBONEB8cJT3txQQwVjA54KO8pTuctT1/KT8D55+sMP37ZXkr9+v5RkIPpBNSAV0f087oNz5fWAwbfjO9mbgARAxJf+T5xlogf/s1HfH2LeLGJnM53bm52NQ==
n  = de69380889250e606a0947e19d1c3da94116af9fdfcb7638ebe190b153c4f94c9e1673f53b455a6bedb497e97b9aa77686d45962cf6cb952fe79ccd64548077f506c26c5198bd20bf79ef34bc0f9cc9e7bc84ca818ee72f96a4fa6701783f6e761bd62ee111cf5000ae900b664b7d42b469be2944df6641ca6ecee5fc12920c97b39cd5484d185f8959ebefa0c975d8448b8c1116c930b3e7665fd2f6e73480b0d2a5c139c0c23e304e34407c7094f7b71410c158c0e7828ef294ee72d4f5fca4fc0f9e7eb0c3f7ed95e4afdfafe519083e904d4805747f4f3ba0dcf97d60306df8cef666e001103125ff93e7196881ffecd477c7d8b78b1899cce776e6e7635
d  = 6affc1c6517c735b2d0843fda5a115851cdc3627baf9de696a09bd9ba20a6c4950f0ca5e9b62352b2153b595cd324d0877bb2739a342d231bc5d8a8f370b7d17e1eff602f2d4527c1167f5f266e7866a6470ab785f6e300010063bb3478ce436d2cdd30f674a4010022fb0790f216540609ad439b6fc2793a536ed4341e17074b7e420e2a91a8eb02b3db43cc1c238d91120b892040c19a383af7712142a4ad914d7d69ee921dfc6c95786d93d2b65b3472318c4155263837d4d192bf2d2b1bbc0d3d31e1c12aa1e51aea83d0815825722380363eea1b69665b55f942f84c5c2dd387ccf2c375c118cbdfb630a0652ccd68b9c9fc6bbe815a6f4196266fea16d
p  = ff77390d004f6f25a7491d0b084bf1906d063502c86dd8d225ad5a424b66afebdd8f29c17760a7b484e106183338d015a72224b3eff5321092c300cc40e88ccbdc254539aa6dd37a9ef37ffc652493075dcf26105c8299c62b322ff9a208e8b9f0e6d15775da4f17d695c046618eb9f0747151f3ea20af82181983291a5cba53
q  = dee04c6629254b008c0646d64f23f2a50efd77dfcd878d16b3595d35321561222b17aa351c21404d928b058a25fa07ad9777da955ef47a3f6ce6dd95aa85da398c87ca00c70e2d64107ae71a2b29f83e76073f48810785e27d7180ca2fa3045d535e594d197f842fde8a8a4924048d0890636b68cd2217e8551acf4085abcc57
dp = 7954907e0245b7ce34535f3ddc24e587e89fe811784be38c7b6487d7b0a406ad893ef35ff568ac7dea164da4b4bf28e4ffca11907c49020c2908009b01c0f10de972b21c1f3ba61338e70144486d447f5d288335861a691a5b6daca76d33ba0485cbba65458df7bea4a5defa0e5f912408c9c1588d3c14df3ebeef325858ee0d
dq = 3f3b0151c680970bbbae1830bad92700df04d81a7d73e47a393db42ce05da2661035b18b49c4c13a80c5c2379458a00f06b6f1238981f0a0c7e3a39fcf761418599f389709e222cc8dc460865312a02410563404fcde4684a0343066ac272298db8b1be8bcc0299f6f127cf30d2de15939a58b167b841da64366ccbfb06cb217
iq = 2c53563807ba049acb5b79d3cd8fc9930b3c176a852a985031a95a230a5509af8adf14f57366cff826250f06087e2b862a77f8e3124e2dd0c233d8b731092a265918faabe7c6fa63df95d553a2d760a3a930c27c8573420576d14f8691848491d0005a45d620fc4fac8806fcc2aea6d1da1e5d2831495493339649805a58f577
'@
$keyB = @'
keyId = lic-2027a
public = UlNBMQAIAAADAAAAAAEAAAAAAAAAAAAAAQAB7K/xbzKOy4omwlDgauf+FYhG+NsiFdehrrq7rhIwb/qcoXp/flf+2mlz1iKD97XpWsobUgZpIeqmMDnhg/S6po3YLkFkuaukhIUASFgx4HMSIwoRU/ZRFN+BEulC7cSIjrIHXkDzU3BDWBBn6V3qyIubVk3ciKNiytP3faaxx4A0Qev3aj9Pg/QfpKz49jpxKpZMfsMhcFp7aZZvlCLzuGEz4y5/UdWB5s3DaEbEURDmAGkRvPCG8jN0mlaVLJOyzQInLXidm7W5LBn+C8N6gbPrbRFLg46gTNKsvcEUHoVuFYsX0yg7pqx+r6Sw61e7eg4NVK7PNtE1SH2bdYpPdQ==
n  = ecaff16f328ecb8a26c250e06ae7fe158846f8db2215d7a1aebabbae12306ffa9ca17a7f7e57feda6973d62283f7b5e95aca1b52066921eaa63039e183f4baa68dd82e4164b9aba4848500485831e07312230a1153f65114df8112e942edc4888eb2075e40f3537043581067e95deac88b9b564ddc88a362cad3f77da6b1c7803441ebf76a3f4f83f41fa4acf8f63a712a964c7ec321705a7b69966f9422f3b86133e32e7f51d581e6cdc36846c45110e6006911bcf086f233749a56952c93b2cd02272d789d9bb5b92c19fe0bc37a81b3eb6d114b838ea04cd2acbdc1141e856e158b17d3283ba6ac7eafa4b0eb57bb7a0e0d54aecf36d135487d9b758a4f75
d  = 35fdf4cf6ef3100f55c421faefa83857e51c7b8c517c361255ef96fca07d982dcd2930ee807dc64f409b021021969252c7392f7e48a416ee96368e1d99a26c29ae55d0476e98c444e54e09c0770df9ab8649a3f44bf8f68a126a394c28ce6074de485251f7f309e7a41524b8ef8d530fa87928f8de03f57185df50a7c32ceedb29bd5be1ca3b7b7b3ca3a8f73c9564760a41f0bb233144f1874ebd23932b97d83adcf2821b747d9dbdd2947942bcf0d5241be738a71ff956e8d49baf28abacc125edfc31f755409f3596c67b6a87a9f7819fe4bb09adf63166ac070ddd533f0cc4f0a28681f6861a960e59fc95564d62818fbebfa9a478ade61210d97567a789
p  = fadf3a544cf1694e9c82f3644565fba39023e38fa860f74406d9e7479c2b3e765cb13c65ec539242a1b9c1473131264a1da3872185cd67b362968f6972da68ee834809f40b5b808d14b6d00a9a45c727833a2c3914ad4287ceeca1f8fbce1396bdb2d2a56e6629878db012467ea4cf55aebbf13141a3cb79144d91258779048f
q  = f1867d2f40d4d6da69bf75e22c11d1e7e4a097c4200da1595ee0c64d53cca43d6a94fc13709936d877ee2021fcfb5231a728bc302646d5905b3383fec3f471074c7288e636a59887ea52c42489721dcec691d79ecef284f5196d1e6db4fcad9b920d7411bd22198000f622b630950cc8db23872c040fcf717e4e211b7197d5bb
dp = 2c51e782211dbd16f03b8a383ab26ba457c09f612fc401aaf8f2e2131a99e12d13ef13d685cbb5d923ad27622664a279118764461dd74decb90c1445f2ce771f3bfe2c13fb35ad1e3f7c94806b04cccc4438b0bb5cd5c71d2b074f0e279e1e83d0fb988f3553c31e797f939af4a0b52853f84c0201b0a82fb9d08245f90afe2d
dq = 60bf4aadfefe43024291db1aba24793cb6ac4d9ed6681f55b957b8978915d44475c93020695e364607e46f22ae3fcdf0edd0b7716223c5b08997dd2bd43aba7bfdf773372474b8e0d8d046998c2516239a0f199c17b1b5e8a842b989a09206c8e67c551e35f0cede238474eb1e12d6882e9fd0a9a506e045ca1e69cca077fdb9
iq = 869983310d78de2e0d68aea2277aa51cd5ead100ef31187f9545d25d359ab11f927c05bd3bf940464fa1a9cb6773156412cff38eaa8a027e4cea3ce819d260b11893553b02542759fe0b110d57c554f8fb2235da38c0a3c77bbd36976b3c9fac0e6d81d45d09fe28215be42cf97f42743c746fd9baacac6079e03d33e3598181
'@

function Read-Key([string]$text) {
    $fields=@{}
    foreach ($line in ($text -split "?
")) {
        if ($line -match '^(\w+)\s*=\s*(\S+)\s*$') { $fields[$Matches[1]]=$Matches[2] }
    }
    return $fields
}
function HexToBytes([string]$hex) {
    $bytes=New-Object byte[] ($hex.Length/2)
    for ($i=0;$i -lt $bytes.Length;$i++){ $bytes[$i]=[Convert]::ToByte($hex.Substring($i*2,2),16) }
    return ,$bytes
}
function New-Rsa([hashtable]$k) {
    $rsa=[System.Security.Cryptography.RSA]::Create()
    $p=New-Object System.Security.Cryptography.RSAParameters
    $p.Modulus=HexToBytes $k['n']; $p.Exponent=[byte[]](1,0,1); $p.D=HexToBytes $k['d']
    $p.P=HexToBytes $k['p']; $p.Q=HexToBytes $k['q']
    $p.DP=HexToBytes $k['dp']; $p.DQ=HexToBytes $k['dq']; $p.InverseQ=HexToBytes $k['iq']
    $rsa.ImportParameters($p)
    return $rsa
}
function Utf8([string]$s) { return [System.Text.UTF8Encoding]::new($false).GetBytes($s) }

$rsaA=New-Rsa (Read-Key $keyA)
$rsaB=New-Rsa (Read-Key $keyB)
$pubA=(Read-Key $keyA)['public']
$pubB=(Read-Key $keyB)['public']

# payload：键按字典序、行尾 LF、UTF-8 无 BOM（设计 3.3）
function Build-Payload([hashtable]$pairs) {
    $lines=@()
    foreach ($k in ($pairs.Keys | Sort-Object)) { $lines += ("$k=" + $pairs[$k]) }
    return ($lines -join "
")
}
function Build-Container([string]$payload,[object]$rsa,[string]$keyId) {
    $payloadBytes=Utf8 $payload
    $sig=$rsa.SignData($payloadBytes,[System.Security.Cryptography.HashAlgorithmName]::SHA256,[System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $payloadB64=[Convert]::ToBase64String($payloadBytes)
    $sigB64=[Convert]::ToBase64String($sig)
    $xml='<?xml version="1.0" encoding="utf-8"?>' + "
" +
        '<NotepadccLicense>' + "
" +
        '  <Payload encoding="base64">' + $payloadB64 + '</Payload>' + "
" +
        '  <Signature alg="RSA-2048-PKCS1-SHA256" keyId="' + $keyId + '">' + $sigB64 + '</Signature>' + "
" +
        '</NotepadccLicense>' + "
"
    return @{ Xml=$xml; Payload=$payload }
}

$base=@{schema='1'; product='notepadcc'; licenseId='8f7c1a2e-4b6d-4c0a-9e21-5f3a7c9b1d44';
    edition='pro'; seats='5'; holder='测试用户'; issuedAt='2026-10-17T00:00:00Z';
    expiresAt='2030-01-01T00:00:00Z'; updateUntil='2027-10-17T00:00:00Z';
    features='compare,datatools,multicaret'; fingerprintMode='none'; channel='stable'}

function Variant([hashtable]$overrides,[string]$removeKey) {
    $pairs=@{}
    foreach ($k in $base.Keys) { $pairs[$k]=$base[$k] }
    foreach ($k in $overrides.Keys) { $pairs[$k]=$overrides[$k] }
    if ($removeKey) { $pairs.Remove($removeKey) }
    return $pairs
}

$items=[ordered]@{}
$items['kLicensePro']        = Build-Container (Build-Payload (Variant @{} $null)) $rsaA 'lic-2026a'
$items['kLicenseExpired']    = Build-Container (Build-Payload (Variant @{issuedAt='2019-01-01T00:00:00Z'; expiresAt='2020-01-01T00:00:00Z'; updateUntil='2020-01-01T00:00:00Z'} $null)) $rsaA 'lic-2026a'
$items['kLicenseNotYetValid']= Build-Container (Build-Payload (Variant @{notBefore='2030-01-01T00:00:00Z'; expiresAt='2031-01-01T00:00:00Z'} $null)) $rsaA 'lic-2026a'
$items['kLicenseSchemaTooNew']=Build-Container (Build-Payload (Variant @{schema='2'} $null)) $rsaB 'lic-2027a'
$items['kLicenseEnterpriseSite']=Build-Container (Build-Payload (Variant @{edition='enterprise'; seats='0'; holder='某某科技有限公司'; holderId='91310000MA1FL0000X'; features='datatools,manageddeploy,teamconfig'; restrictions='noncommercial'} $null)) $rsaA 'lic-2026a'
$items['kLicenseWrongProduct']= Build-Container (Build-Payload (Variant @{product='notepadccx'} $null)) $rsaA 'lic-2026a'
$items['kLicenseUnknownFeature']=Build-Container (Build-Payload (Variant @{features='multicaret,quantumworth'} $null)) $rsaA 'lic-2026a'
$items['kLicenseMissingFeatures']=Build-Container (Build-Payload (Variant @{} 'features')) $rsaA 'lic-2026a'
$items['kLicenseFingerprintBound']=Build-Container (Build-Payload (Variant @{fingerprintMode='tolerant'; fingerprint='K7M2P-9XQ4T-3BNWD-8HJ6R'} $null)) $rsaA 'lic-2026a'

$sb=New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// 许可测试固定件（由 tools/make_license_fixtures.ps1 生成，勿手改）。')
[void]$sb.AppendLine('// 私钥是一次性开发钥匙，只用来给测试许可签名；固定时间戳与字段值被')
[void]$sb.AppendLine('// tests/unit/test_main.cpp 的"许可验签"一段逐条断言，改动必须两边同步。')
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('namespace npcc::test {')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// kLicensePro 的 payload 原文（键按字典序、LF）——序列化往返比对用')
[void]$sb.AppendLine('inline constexpr const char* kLicenseProPayload = R"pl(' + $items['kLicensePro'].Payload + ')pl";')
[void]$sb.AppendLine('')
foreach ($name in $items.Keys) {
    [void]$sb.AppendLine('inline constexpr const char* ' + $name + ' = R"lic(' + $items[$name].Xml + ')lic";')
    [void]$sb.AppendLine('')
}
[void]$sb.AppendLine('}  // namespace npcc::test')
[void]$sb.AppendLine('')
[IO.File]::WriteAllText('D:\codepad\codepad\tests\unit\LicenseFixtures.h',$sb.ToString(),[System.Text.UTF8Encoding]::new($false))
Write-Output ('fixtures: ' + $items.Count)
Write-Output ('publicA=' + $pubA)
Write-Output ('publicB=' + $pubB)

if ($WriteKeysHeader) {
    $k=New-Object System.Text.StringBuilder
    [void]$k.AppendLine('// 许可验签的内置公钥表（设计《notepadcc许可与激活设计.md》3.4）。由')
    [void]$k.AppendLine('// tools/make_license_fixtures.ps1 生成（开发钥匙）；正式发布前用离线生成的')
    [void]$k.AppendLine('// 真钥匙替换这里、并同步重跑 tools/make_license_fixtures.ps1 更新测试固定件。')
    [void]$k.AppendLine('// 至少能同时装两把（当前 + 下一把），换钥期间旧许可仍可用。')
    [void]$k.AppendLine('#pragma once')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('#include <string_view>')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('namespace npcc::license {')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('struct LicenseKeyEntry {')
    [void]$k.AppendLine('    std::string_view keyId;')
    [void]$k.AppendLine('    std::string_view publicKeyBase64;   // BCrypt RSAPUBLIC_BLOB')
    [void]$k.AppendLine('};')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('inline constexpr LicenseKeyEntry kLicenseKeys[] = {')
    [void]$k.AppendLine('    {"lic-2026a", "' + $pubA + '"},')
    [void]$k.AppendLine('    {"lic-2027a", "' + $pubB + '"},')
    [void]$k.AppendLine('};')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('// 找不到返回空 —— 调用方按 KeyUnknown 处理（提示升级版本，见设计 9）')
    [void]$k.AppendLine('inline std::string_view FindLicensePublicKey(std::string_view keyId) {')
    [void]$k.AppendLine('    for (const LicenseKeyEntry& entry : kLicenseKeys) {')
    [void]$k.AppendLine('        if (entry.keyId == keyId) {')
    [void]$k.AppendLine('            return entry.publicKeyBase64;')
    [void]$k.AppendLine('        }')
    [void]$k.AppendLine('    }')
    [void]$k.AppendLine('    return {};')
    [void]$k.AppendLine('}')
    [void]$k.AppendLine('')
    [void]$k.AppendLine('}  // namespace npcc::license')
    [void]$k.AppendLine('')
    [IO.File]::WriteAllText('D:\codepad\codepad\src\license\LicenseKeys.h',$k.ToString(),[System.Text.UTF8Encoding]::new($false))
    Write-Output 'LicenseKeys.h written'
}
