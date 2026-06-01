from http import accept_header, connection_header, create_folder_header, read_boundary, read_header_value, request, print_after_upload_header, overwrite_file_header, authorization_header, upgrade_header, ws_version_header

if __name__ == "__main__":
    want_headers = {
        'X-Api-Key': read_header_value('XApiKey'),
        'Authorization': authorization_header(),
        'Content-Length': read_header_value('ContentLength'),
        'If-None-Match': read_header_value('IfNoneMatch'),
        'Print-After-Upload': print_after_upload_header(),
        'Overwrite': overwrite_file_header(),
        'Create-Folder': create_folder_header(),
        'Content-Type': read_boundary(),
        'Connection': connection_header(),
        'Accept': accept_header(),
        'Upgrade': upgrade_header(),
        'Sec-WebSocket-Key': read_header_value('SecWebSocketKey'),
        'Sec-WebSocket-Version': ws_version_header(),
    }
    http, final = request(want_headers)
    compiled = http.compile("nhttp::parser::request")
    with open("http_req_automaton.h", "w") as header:
        header.write(compiled.cpp_header())
    with open("http_req_automaton.cpp", "w") as file:
        file.write(compiled.cpp_file())
