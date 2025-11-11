# Importa a biblioteca web Flask
from flask import Flask, request, jsonify

# O token que o servidor ESPERA receber.
# DEVE ser exatamente o mesmo token que você colocou no src/main.cpp
EXPECTED_TOKEN = "e83z9XyJk4mFpA6hD7qWc2sT1uVb0R3g"

# Cria a aplicação Flask
app = Flask(__name__)

# Define a rota (endpoint) que o ESP32 vai chamar
# O caminho deve ser o mesmo que em SERVER_ATTENDANCE_PATH
@app.route('/api/v1/attendance', methods=['POST'])
def handle_attendance():
    print("-" * 50)
    print("REQUISIÇÃO RECEBIDA DO ESP32-CAM")
    
    # 1. VALIDAÇÃO DO AUTH_BEARER_TOKEN
    auth_header = request.headers.get('Authorization')
    
    if auth_header and auth_header.startswith("Bearer "):
        received_token = auth_header.split(" ")[1]
        
        if received_token != EXPECTED_TOKEN:
            print("ERRO DE AUTORIZAÇÃO: Token recebido NÃO CONFERE.")
            print(f"Esperado: {EXPECTED_TOKEN}")
            print(f"Recebido: {received_token}")
            # Retorna 401: Não Autorizado
            return jsonify({"status": "error", "message": "Token inválido"}), 401
    else:
        print("ERRO DE AUTORIZAÇÃO: Header 'Authorization' faltando.")
        return jsonify({"status": "error", "message": "Header de autorização faltando"}), 401

    print("Status: Token Válido (200 OK)")

    # 2. PROCESSAMENTO DO PAYLOAD
    try:
        data = request.get_json()
        
        # O ESP32 envia dados JSON, incluindo a foto Base64
        print(f"Timestamp Recebido: {data.get('ts')}")
        print(f"Sala Recebida: {data.get('room')}")
        
        # A imagem é grande, então só vamos mostrar o tamanho dela
        img_b64 = data.get('image_b64', '')
        print(f"Tamanho da Imagem Base64: {len(img_b64)} caracteres.")

        # 3. Resposta de Sucesso para o ESP32
        # É ESSENCIAL responder com 200 para o ESP32 não tentar enviar o dado offline.
        return jsonify({
            "status": "Success",
            "message": "Ponto registrado com sucesso no backend de teste."
        }), 200

    except Exception as e:
        print(f"Erro ao processar JSON: {e}")
        # Retorna 500: Erro Interno do Servidor
        return jsonify({"status": "error", "message": "Erro interno do servidor"}), 500

if __name__ == '__main__':
    # Roda o servidor no IP 0.0.0.0 (para escutar em todas as interfaces) 
    # na porta 3000 (a porta configurada no ESP32)
    print("\n--- INICIANDO SERVIDOR DE TESTE ---")
    print("Aguardando requisições POST em http://SEU_IP:3000/api/v1/attendance")
    print("----------------------------------\n")
    app.run(host='172.24.153.47', port=3000, debug=True)

