{protocol_docs}
template <typename D, typename Base = internal::base_mixin>
class {protocol_name}Protocol : public Base {{
public:
    {protocol_name}Protocol() {{
        internal::Check{protocol_name}ProtocolSubclass<D>();
{constructor_definition}
    }}

protected:
    {protocol_name_snake}_protocol_ops_t {protocol_name_snake}_protocol_ops_ = {{}};

private:
{protocol_definitions}
}};

class {protocol_name}ProtocolClient {{
public:
    {protocol_name}ProtocolClient()
        : ops_(nullptr), ctx_(nullptr) {{}}
    {protocol_name}ProtocolClient(const {protocol_name_snake}_protocol_t* proto)
        : ops_(proto->ops), ctx_(proto->ctx) {{}}

    {protocol_name}ProtocolClient(zx_device_t* parent) {{
        {protocol_name_snake}_protocol_t proto;
        if (device_get_protocol(parent, ZX_PROTOCOL_{protocol_name_uppercase}, &proto) == ZX_OK) {{
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        }} else {{
            ops_ = nullptr;
            ctx_ = nullptr;
        }}
    }}

    {protocol_name}ProtocolClient(CompositeProtocolClient& composite, const char* fragment_name) {{
        zx_device_t* fragment;
        bool found = composite.GetFragment(fragment_name, &fragment);
        {protocol_name_snake}_protocol_t proto;
        if (found && device_get_protocol(fragment, ZX_PROTOCOL_{protocol_name_uppercase}, &proto) == ZX_OK) {{
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        }} else {{
            ops_ = nullptr;
            ctx_ = nullptr;
        }}
    }}

    {protocol_name}ProtocolClient(zx_device_t* parent, const char* fragment_name) {{
        zx_device_t* fragment;
        bool found = device_get_fragment(parent, fragment_name, &fragment);
        {protocol_name_snake}_protocol_t proto;
        if (found && device_get_protocol(fragment, ZX_PROTOCOL_{protocol_name_uppercase}, &proto) == ZX_OK) {{
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        }} else {{
            ops_ = nullptr;
            ctx_ = nullptr;
        }}
    }}

    // Create a {protocol_name}ProtocolClient from the given parent device + "fragment".
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent,
                                        {protocol_name}ProtocolClient* result) {{
        {protocol_name_snake}_protocol_t proto;
        zx_status_t status = device_get_protocol(
                parent, ZX_PROTOCOL_{protocol_name_uppercase}, &proto);
        if (status != ZX_OK) {{
            return status;
        }}
        *result = {protocol_name}ProtocolClient(&proto);
        return ZX_OK;
    }}

    // Create a {protocol_name}ProtocolClient from the given parent device.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent, const char* fragment_name,
                                        {protocol_name}ProtocolClient* result) {{
        zx_device_t* fragment;
        bool found = device_get_fragment(parent, fragment_name, &fragment);
        if (!found) {{
          return ZX_ERR_NOT_FOUND;
        }}
        return CreateFromDevice(fragment, result);
    }}

    // Create a {protocol_name}ProtocolClient from the given composite protocol.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromComposite(CompositeProtocolClient& composite,
                                           const char* fragment_name,
                                           {protocol_name}ProtocolClient* result) {{
        zx_device_t* fragment;
        bool found = composite.GetFragment(fragment_name, &fragment);
        if (!found) {{
          return ZX_ERR_NOT_FOUND;
        }}
        return CreateFromDevice(fragment, result);
    }}

    void GetProto({protocol_name_snake}_protocol_t* proto) const {{
        proto->ctx = ctx_;
        proto->ops = ops_;
    }}
    bool is_valid() const {{
        return ops_ != nullptr;
    }}
    void clear() {{
        ctx_ = nullptr;
        ops_ = nullptr;
    }}

{client_definitions}
private:
    {protocol_name_snake}_protocol_ops_t* ops_;
    void* ctx_;
}};
