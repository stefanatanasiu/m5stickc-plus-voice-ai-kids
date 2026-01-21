// Azure Bicep template for M5Voice sketch
// Creates: Azure OpenAI account + deployments (gpt-5.2, whisper-1)
//          Storage Account + Table for logging and an Account SAS output

param prefix string = 'm5voice'
param location string = resourceGroup().location
param openAiSkuName string = 'S0'
param storageSku string = 'Standard_LRS'
param tableName string = 'm5voiceLogs'
param sasExpiry string = '2030-01-01T00:00:00Z' // ISO8601 expiry for generated SAS
param sasStart string = utcNow('yyyy-MM-ddTHH:mm:ssZ') // ISO8601 format for SAS start

// Azure OpenAI account
resource openAi 'Microsoft.CognitiveServices/accounts@2024-10-01' = {
  name: '${prefix}-openai'
  location: location
  kind: 'OpenAI'
  sku: {
    name: openAiSkuName
  }
  properties: {
    publicNetworkAccess: 'Enabled'
  }
}

// Deploy GPT-5.2
resource gptDeployment 'Microsoft.CognitiveServices/accounts/deployments@2024-10-01' = {
  parent: openAi
  name: 'gpt-5-2-deployment'
  properties: {
    model: {
      format: 'OpenAI'
      name: 'gpt-4o'
      version: '2024-11-20'
    }
  }
  sku: {
    name: 'Standard'
    capacity: 1
  }
}

// Deploy Whisper-1 (speech-to-text) - depends on gptDeployment to avoid conflict
resource whisperDeployment 'Microsoft.CognitiveServices/accounts/deployments@2024-10-01' = {
  parent: openAi
  name: 'whisper-1-deployment'
  properties: {
    model: {
      format: 'OpenAI'
      name: 'whisper'
      version: '001'
    }
  }
  sku: {
    name: 'Standard'
    capacity: 1
  }
  dependsOn: [gptDeployment]
}

// Storage Account for logging
var storageAccountName = toLower(take('${prefix}sa${uniqueString(resourceGroup().id)}', 24))
resource storage 'Microsoft.Storage/storageAccounts@2023-05-01' = {
  name: storageAccountName
  location: location
  sku: {
    name: storageSku
  }
  kind: 'StorageV2'
  properties: {
    accessTier: 'Hot'
  }
}

// Table service and table resource
resource tableService 'Microsoft.Storage/storageAccounts/tableServices@2023-05-01' = {
  parent: storage
  name: 'default'
}

resource table 'Microsoft.Storage/storageAccounts/tableServices/tables@2023-05-01' = {
  parent: tableService
  name: tableName
}

// Generate an account SAS (account-level) for Table service. You may adjust permissions and expiry.
var accountSas = storage.listAccountSas('2023-05-01', {
  signedServices: 't'
  signedResourceTypes: 'sco'
  signedPermission: 'raud'
  signedProtocol: 'https'
  signedStart: sasStart
  signedExpiry: sasExpiry
})

output openAiAccountName string = openAi.name
output openAiEndpoint string = 'https://${openAi.name}.openai.azure.com'
output gptDeploymentName string = gptDeployment.name
output whisperDeploymentName string = whisperDeployment.name
output storageAccountName string = storage.name
output tableNameOutput string = table.name
output tableSasUrl string = 'https://${storage.name}.table.${environment().suffixes.storage}/${table.name}?${accountSas.accountSasToken}'

// Notes (not enforced):
// - Ensure the subscription/region supports Azure OpenAI and you have access to create it.
// - You may need to approve or request quota for the OpenAI SKU in your subscription.
// - Adjust SAS expiry and permissions as required for your security posture.
